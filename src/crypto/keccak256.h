// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_CRYPTO_KECCAK256_H
#define B3COIN_CRYPTO_KECCAK256_H

#include <cstddef>
#include <cstdint>
#include <span>

/**
 * Keccak-256 as used by Ethereum: the pre-standard Keccak padding (domain
 * byte 0x01) over the same Keccak-f[1600] permutation and 1088-bit rate as
 * SHA3-256. NOT interchangeable with crypto/sha3.h (SHA3 pads with 0x06).
 * Introduced for the B3 finality validator-set commitment, whose leaves,
 * header hash and members_root are Keccak so that an Ethereum verifier can
 * recompute them with its native hash.
 */
class Keccak256
{
private:
    uint64_t m_state[25] = {0};
    unsigned char m_buffer[8];
    unsigned m_bufsize = 0;
    unsigned m_pos = 0;

    static constexpr unsigned RATE_BITS = 1088;
    static constexpr unsigned RATE_BUFFERS = RATE_BITS / (8 * sizeof(m_buffer));
    static_assert(RATE_BITS % (8 * sizeof(m_buffer)) == 0, "Rate must be a multiple of 8 bytes");

public:
    static constexpr size_t OUTPUT_SIZE = 32;

    Keccak256() = default;
    Keccak256& Write(std::span<const unsigned char> data);
    Keccak256& Finalize(std::span<unsigned char> output);
    Keccak256& Reset();
};

#endif // B3COIN_CRYPTO_KECCAK256_H
