// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_TEST_UTIL_LEGACY_POW_H
#define B3COIN_TEST_UTIL_LEGACY_POW_H

#include <arith_uint256.h>
#include <primitives/block.h>
#include <sync.h>
#include <uint256.h>

#include <cstdint>
#include <map>

namespace b3test {

/** Grind the legacy (scrypt) proof of work for `header` from nonce 0 and
 * return the first nonce whose hash meets `header.nBits`.
 *
 * The fixtures build the same deterministic legacy prefix in every test
 * case, and one Debug scrypt search costs tens of seconds per block, so the
 * nonce found for a header is remembered for the rest of the process. A
 * remembered nonce is re-verified before it is reused: a stale entry costs
 * one extra hash, never a wrong block. */
inline uint32_t MineLegacyProofOfWork(CBlockHeader header)
{
    static Mutex mutex;
    static std::map<uint256, uint32_t> found GUARDED_BY(mutex);
    header.nNonce = 0;
    // SHA-256d of the nonce-0 header: cheap, and specific to every other field.
    const uint256 key{header.GetHash()};
    const arith_uint256 target{arith_uint256().SetCompact(header.nBits)};
    const auto meets = [&] { return UintToArith256(header.GetLegacyB3Hash()) <= target; };
    {
        LOCK(mutex);
        if (const auto it{found.find(key)}; it != found.end()) {
            header.nNonce = it->second;
            if (meets()) return header.nNonce;
            header.nNonce = 0;
        }
    }
    while (!meets()) ++header.nNonce;
    LOCK(mutex);
    found[key] = header.nNonce;
    return header.nNonce;
}

} // namespace b3test

#endif // B3COIN_TEST_UTIL_LEGACY_POW_H
