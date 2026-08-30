// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/b3fixed.h>

#include <util/int128.h>

#include <QChar>

namespace B3Fixed {

QString format(int64_t amount, int decimals)
{
    const bool negative = amount < 0;
    uint64_t magnitude = negative ? -static_cast<uint64_t>(amount) : static_cast<uint64_t>(amount);
    uint64_t divisor = 1;
    for (int i = 0; i < decimals; ++i) divisor *= 10;
    const uint64_t whole = divisor > 1 ? magnitude / divisor : magnitude;
    const uint64_t frac = divisor > 1 ? magnitude % divisor : 0;

    QString text = QString::number(whole);
    for (int pos = text.size() - 3; pos > 0; pos -= 3) {
        text.insert(pos, QChar(0x2009));
    }
    if (decimals > 0) {
        text += QLatin1Char('.') + QString::number(frac).rightJustified(decimals, QLatin1Char('0'));
    }
    return negative ? QLatin1Char('-') + text : text;
}

std::optional<int64_t> mulScaled(int64_t a, int64_t b, int b_decimals)
{
    util::Signed128 scale = 1;
    for (int i = 0; i < b_decimals; ++i) scale *= 10;
    if (scale == 0) return std::nullopt;
    const util::Signed128 product = static_cast<util::Signed128>(a) *
                                    static_cast<util::Signed128>(b) / scale;
    if (product > INT64_MAX || product < INT64_MIN) return std::nullopt;
    return static_cast<int64_t>(product);
}

} // namespace B3Fixed
