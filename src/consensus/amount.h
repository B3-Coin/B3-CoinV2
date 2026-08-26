// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_AMOUNT_H
#define BITCOIN_CONSENSUS_AMOUNT_H

#include <cstdint>

/** Amount in B3Coin's smallest unit (Can be negative) */
typedef int64_t CAmount;

/** The amount of atomic units in one B3Coin. */
static constexpr CAmount COIN = 1000000;
/** The MODERN display unit (locked denomination model): 1 B3 = 1,000 legacy
 *  COIN = 1e9 base units. All human-facing amounts (RPC, GUI, config args)
 *  read and write B3; consensus stays in base units. Matches the finality
 *  weight unit (one whole B3 = one snapshot weight). */
static constexpr CAmount KILO_COIN = 1000000000;

/** No amount larger than this (in satoshi) is valid.
 *
 * Note that this constant is *not* the total money supply, which in Bitcoin
 * currently happens to be less than 21,000,000 BTC for various reasons, but
 * rather a sanity check. As this sanity check is used by consensus-critical
 * validation code, the exact value of the MAX_MONEY constant is consensus
 * critical; in unusual circumstances like a(nother) overflow bug that allowed
 * for the creation of coins out of thin air modification could lead to a fork.
 * */
static constexpr CAmount MAX_MONEY = 662200000000 * COIN;
inline bool MoneyRange(const CAmount& nValue) { return (nValue >= 0 && nValue <= MAX_MONEY); }

/**
 * Stock Bitcoin monetary values, retained for the non-B3 test chains and
 * for upstream generic vectors. B3 consensus uses COIN and MAX_MONEY above;
 * these exist so the retained Bitcoin test chains keep their original
 * semantics without touching B3's historical constants.
 */
static constexpr CAmount BITCOIN_COIN = 100000000;
static constexpr CAmount GENERIC_MAX_MONEY = 21000000 * BITCOIN_COIN;

#endif // BITCOIN_CONSENSUS_AMOUNT_H
