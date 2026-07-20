// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_LEGACY_SCRYPT_H
#define B3COIN_LEGACY_SCRYPT_H

#include <span.h>
#include <uint256.h>

namespace legacy {

/** B3Coin's original scrypt_1024_1_1_256 proof-of-work hash. */
uint256 ScryptHash(std::span<const unsigned char> input);

} // namespace legacy

#endif // B3COIN_LEGACY_SCRYPT_H
