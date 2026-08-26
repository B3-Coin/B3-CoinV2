// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <crypto/keccak256.h>

#include <crypto/common.h>
#include <crypto/sha3.h> // KeccakF

#include <algorithm>
#include <cassert>

// The absorb loop mirrors SHA3_256::Write byte-for-byte; the only difference
// between the two constructions is the padding domain byte in Finalize
// (Keccak 0x01 vs SHA3 0x06).
Keccak256& Keccak256::Write(std::span<const unsigned char> data)
{
    if (m_bufsize && data.size() >= sizeof(m_buffer) - m_bufsize) {
        std::copy(data.begin(), data.begin() + (sizeof(m_buffer) - m_bufsize), m_buffer + m_bufsize);
        data = data.subspan(sizeof(m_buffer) - m_bufsize);
        m_state[m_pos++] ^= ReadLE64(m_buffer);
        m_bufsize = 0;
        if (m_pos == RATE_BUFFERS) {
            KeccakF(m_state);
            m_pos = 0;
        }
    }
    while (data.size() >= sizeof(m_buffer)) {
        m_state[m_pos++] ^= ReadLE64(data.data());
        data = data.subspan(8);
        if (m_pos == RATE_BUFFERS) {
            KeccakF(m_state);
            m_pos = 0;
        }
    }
    if (data.size()) {
        std::copy(data.begin(), data.end(), m_buffer + m_bufsize);
        m_bufsize += data.size();
    }
    return *this;
}

Keccak256& Keccak256::Finalize(std::span<unsigned char> output)
{
    assert(output.size() == OUTPUT_SIZE);
    std::fill(m_buffer + m_bufsize, m_buffer + sizeof(m_buffer), 0);
    m_buffer[m_bufsize] ^= 0x01; // Keccak (pre-FIPS-202) domain/padding byte
    m_state[m_pos] ^= ReadLE64(m_buffer);
    m_state[RATE_BUFFERS - 1] ^= 0x8000000000000000;
    KeccakF(m_state);
    for (unsigned i = 0; i < 4; ++i) {
        WriteLE64(output.data() + 8 * i, m_state[i]);
    }
    return *this;
}

Keccak256& Keccak256::Reset()
{
    m_bufsize = 0;
    m_pos = 0;
    std::fill(std::begin(m_state), std::end(m_state), 0);
    return *this;
}
