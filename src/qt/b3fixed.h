// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3FIXED_H
#define BITCOIN_QT_B3FIXED_H

#include <QString>

#include <cstdint>
#include <optional>

/**
 * Integer fixed-point helpers for financial values. Amounts are always
 * carried as int64 raw units plus an explicit decimal count; floating
 * point is never a source of truth.
 */
namespace B3Fixed {

//! Render `amount` (raw units) with `decimals` fractional digits,
//! thin-space thousand separators in the whole part.
QString format(int64_t amount, int decimals);

//! Checked multiply of two raw amounts where `b` carries `b_decimals`
//! fractional digits (e.g. total = price × quantity). Returns nullopt on
//! overflow instead of wrapping.
std::optional<int64_t> mulScaled(int64_t a, int64_t b, int b_decimals);

} // namespace B3Fixed

#endif // BITCOIN_QT_B3FIXED_H
