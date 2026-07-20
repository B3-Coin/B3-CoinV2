/*-
 * Copyright 2009 Colin Percival, 2011 ArtForz, 2011 pooler, 2013 Balthazar
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Adapted for B3Coin Core from the original B3Coin generic implementation.
 */

#include <legacy/scrypt.h>

#include <crypto/common.h>
#include <crypto/hmac_sha256.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace legacy {
namespace {

void PBKDF2SHA256OneRound(std::span<const unsigned char> password,
                          std::span<const unsigned char> salt,
                          std::span<unsigned char> output)
{
    std::vector<unsigned char> message;
    message.reserve(salt.size() + 4);
    message.insert(message.end(), salt.begin(), salt.end());

    for (uint32_t block{1}; !output.empty(); ++block) {
        const size_t salt_size{message.size()};
        message.resize(salt_size + 4);
        WriteBE32(message.data() + salt_size, block);

        std::array<unsigned char, CHMAC_SHA256::OUTPUT_SIZE> digest;
        CHMAC_SHA256(password.data(), password.size())
            .Write(message.data(), message.size())
            .Finalize(digest.data());

        const size_t count{std::min(output.size(), digest.size())};
        std::copy_n(digest.begin(), count, output.begin());
        output = output.subspan(count);
        message.resize(salt_size);
    }
}

constexpr uint32_t RotL(uint32_t value, int count)
{
    return (value << count) | (value >> (32 - count));
}

void XorSalsa8(uint32_t* block, const uint32_t* other)
{
    uint32_t x00{block[0] ^= other[0]}, x01{block[1] ^= other[1]};
    uint32_t x02{block[2] ^= other[2]}, x03{block[3] ^= other[3]};
    uint32_t x04{block[4] ^= other[4]}, x05{block[5] ^= other[5]};
    uint32_t x06{block[6] ^= other[6]}, x07{block[7] ^= other[7]};
    uint32_t x08{block[8] ^= other[8]}, x09{block[9] ^= other[9]};
    uint32_t x10{block[10] ^= other[10]}, x11{block[11] ^= other[11]};
    uint32_t x12{block[12] ^= other[12]}, x13{block[13] ^= other[13]};
    uint32_t x14{block[14] ^= other[14]}, x15{block[15] ^= other[15]};

    for (int round{0}; round < 8; round += 2) {
        x04 ^= RotL(x00 + x12, 7); x09 ^= RotL(x05 + x01, 7);
        x14 ^= RotL(x10 + x06, 7); x03 ^= RotL(x15 + x11, 7);
        x08 ^= RotL(x04 + x00, 9); x13 ^= RotL(x09 + x05, 9);
        x02 ^= RotL(x14 + x10, 9); x07 ^= RotL(x03 + x15, 9);
        x12 ^= RotL(x08 + x04, 13); x01 ^= RotL(x13 + x09, 13);
        x06 ^= RotL(x02 + x14, 13); x11 ^= RotL(x07 + x03, 13);
        x00 ^= RotL(x12 + x08, 18); x05 ^= RotL(x01 + x13, 18);
        x10 ^= RotL(x06 + x02, 18); x15 ^= RotL(x11 + x07, 18);

        x01 ^= RotL(x00 + x03, 7); x06 ^= RotL(x05 + x04, 7);
        x11 ^= RotL(x10 + x09, 7); x12 ^= RotL(x15 + x14, 7);
        x02 ^= RotL(x01 + x00, 9); x07 ^= RotL(x06 + x05, 9);
        x08 ^= RotL(x11 + x10, 9); x13 ^= RotL(x12 + x15, 9);
        x03 ^= RotL(x02 + x01, 13); x04 ^= RotL(x07 + x06, 13);
        x09 ^= RotL(x08 + x11, 13); x14 ^= RotL(x13 + x12, 13);
        x00 ^= RotL(x03 + x02, 18); x05 ^= RotL(x04 + x07, 18);
        x10 ^= RotL(x09 + x08, 18); x15 ^= RotL(x14 + x13, 18);
    }

    block[0] += x00; block[1] += x01; block[2] += x02; block[3] += x03;
    block[4] += x04; block[5] += x05; block[6] += x06; block[7] += x07;
    block[8] += x08; block[9] += x09; block[10] += x10; block[11] += x11;
    block[12] += x12; block[13] += x13; block[14] += x14; block[15] += x15;
}

void ScryptCore(std::array<uint32_t, 32>& x)
{
    std::vector<uint32_t> scratch(1024 * 32);
    for (size_t i{0}; i < 1024; ++i) {
        std::copy(x.begin(), x.end(), scratch.begin() + i * 32);
        XorSalsa8(x.data(), x.data() + 16);
        XorSalsa8(x.data() + 16, x.data());
    }
    for (size_t i{0}; i < 1024; ++i) {
        const size_t offset{32 * (x[16] & 1023)};
        for (size_t k{0}; k < x.size(); ++k) x[k] ^= scratch[offset + k];
        XorSalsa8(x.data(), x.data() + 16);
        XorSalsa8(x.data() + 16, x.data());
    }
}

} // namespace

uint256 ScryptHash(std::span<const unsigned char> input)
{
    std::array<unsigned char, 128> initial;
    PBKDF2SHA256OneRound(input, input, initial);

    std::array<uint32_t, 32> x;
    for (size_t i{0}; i < x.size(); ++i) x[i] = ReadLE32(initial.data() + i * 4);
    ScryptCore(x);
    for (size_t i{0}; i < x.size(); ++i) WriteLE32(initial.data() + i * 4, x[i]);

    uint256 result;
    PBKDF2SHA256OneRound(input, initial, std::span<unsigned char>{result.begin(), result.size()});
    return result;
}

} // namespace legacy
