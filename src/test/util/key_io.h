// Copyright (c) 2026 The B3Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_KEY_IO_H
#define BITCOIN_TEST_UTIL_KEY_IO_H

#include <string>
#include <string_view>

class CChainParams;

namespace test {

/**
 * Translate an encoded Bitcoin-main key/address test vector to the equivalent
 * encoding for target_params without changing its key or script payload.
 */
std::string TranslateBitcoinMainKeyIO(std::string_view encoded, const CChainParams& target_params);

/** Translate valid Bitcoin-main WIF tokens embedded in descriptor test text. */
std::string TranslateBitcoinMainWIFs(std::string text, const CChainParams& target_params);

} // namespace test

#endif // BITCOIN_TEST_UTIL_KEY_IO_H
