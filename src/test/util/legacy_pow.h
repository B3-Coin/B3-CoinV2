// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_TEST_UTIL_LEGACY_POW_H
#define B3COIN_TEST_UTIL_LEGACY_POW_H

#include <arith_uint256.h>
#include <primitives/block.h>
#include <sync.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <map>
#include <utility>

namespace b3test {

/** The proof-of-work nonces of the legacy blocks that more than one suite
 * builds (the prefix ModernPosSetup and BindingFixture start from), keyed
 * by the hash of each block's header with nonce 0. Each is the first nonce
 * from 0 whose scrypt hash (N 1024, r 1, p 1) meets the header's nBits,
 * which was checked apart from this code. */
inline constexpr std::array<std::pair<uint256, uint32_t>, 32> KNOWN_LEGACY_NONCES{{
    {uint256{"05cdf8d4f122e362b3b3699005df34fc3a1796e80bfedb9f11d423c0ea6eb341"}, 18569},
    {uint256{"1e9d74e8194f6af828dd6bbe667ec4ef2cfa60f039a19e9f56b4758116a9d454"}, 12075},
    {uint256{"25b249679cceec38628cd8f30638d7f5db87d8e3cdd0f11be8d0b37b01dc777f"}, 61691},
    {uint256{"297efdead3d36a404a77e8fa7ae194af5227fbcc908deb974a67761645d2c46e"}, 2954},
    {uint256{"2b5b9ef181dcd00cc4ee416cb9fb873f7184b6136d33dd1405594d76437b1db1"}, 2311},
    {uint256{"2f93f0c10e9680f1431d4f8626e3ff5c1ca318607bc37561157ace9f9828b771"}, 18547},
    {uint256{"350ac1d8d1130f37a1bbd6ecf836f057a9917aa2c7bf1be2535bb67eeb741fbb"}, 555},
    {uint256{"37bb9882a4866acefb0ae8e60698c42568abc2ec642cd9972a9af0446a183f1a"}, 2851},
    {uint256{"3c29fee627e6ac90092a58a5eb7261b2509e2edd47a944ddc22a00151bdfa223"}, 704},
    {uint256{"439353525ba8880241255d17a59887c8046b5a5b8b8cc87803267a4358ebae07"}, 74580},
    {uint256{"5e757134a39b7c9557276cae2507cb0959ccc77089edc71d11ff3576c8e9b9ae"}, 708},
    {uint256{"61381d79d7f344ab74011ef45cd08d8b0b18f9ba1c66bb6898aa7bcc8d4bbf4e"}, 297520},
    {uint256{"66167bcdcdd80863da68e4a3e00728bb09fe60f95b76bcb9eda3518bc9373b89"}, 3654},
    {uint256{"69986e17b059c12240f54580828e43605aee8ec759353303140cc90048d54882"}, 3382},
    {uint256{"6eeebe5fac33aee354529c7dae9af1ab01a73ab646f010a8a3821b9df8baee25"}, 17949},
    {uint256{"77a178f9e8344b6f7d2bf52a8a96fbd7fdef26e38d726a8f3f4db3f0c522f6dc"}, 13978},
    {uint256{"7d569d4276da60ac7325f511063609ff7018052e040a5f0add0fc902265898c2"}, 14403},
    {uint256{"8c90716b0d3319d17e3cff9ebc7a4580c6147543c4a6b2d7de474a0a1c862ca5"}, 123633},
    {uint256{"92cee14d3a7744db72d5d598bdbf2d9782ff3f616ffaea8c331a7f005b8e2a5d"}, 3},
    {uint256{"9404c8f8a1ad1bf7326fe14f7a0fc858a1e9c24eb305de3875191c55903c90db"}, 21393},
    {uint256{"96a0007b0aa68563fa5673351ab6320cb3d6597d76e6a5a447c5b678bc5776a8"}, 171884},
    {uint256{"99eb69666bfed5a6249cce8ef91641e286b2c9a7cb5ebc230b3aec721c8e8a70"}, 17618},
    {uint256{"9d5f41deeb4850e197e4ff6a4e69a4868c9e9290e2139637197cbaffb2a99597"}, 2},
    {uint256{"a51cc533b766e7dfa1a7fd7d1c19e80e72ec3bd2ab327b256fcd49cea9c76ae7"}, 9893},
    {uint256{"a54082818dcb9dde79262de6fd34ac2ffe60fe55716c92fafb0bfd12a75daa6f"}, 62097},
    {uint256{"ab04314a42be7ed7abb392531158d667c92aba22a599682e67570221ea1bc4d1"}, 1140},
    {uint256{"ae80c7e4322e3927ab3c09ec357971adbf725bebd71c19ac5005d95b40771023"}, 734},
    {uint256{"b06f084806e0629fbcf15bfea49f195b0dccda358bbeb4b849d69dc5a8ba4e11"}, 21215},
    {uint256{"c57c8a31da73c7ad3db61de789a0f043974d974666cfa7aae444b9a2ee357f70"}, 88294},
    {uint256{"c68517c0a593f70418e694856b954f149141e14f4052b940ec7d4af09ede03f2"}, 2170},
    {uint256{"ddf12c03872ba393c7feb0b84c65bfa92cf188e856b592022ca01b487842785c"}, 6219},
    {uint256{"f5f4c92920aada630f3e09b0262db4ae348c897f9a0c5cc1eb20426fe7312973"}, 40129},
}};

/** Grind the legacy (scrypt) proof of work for `header` from nonce 0 and
 * return the first nonce whose hash meets `header.nBits`.
 *
 * The fixtures build the same deterministic legacy blocks in every test
 * case, and searching the prefix alone costs minutes of CPU. So the search
 * starts from KNOWN_LEGACY_NONCES, and remembers the nonce it finds for any
 * other header for the rest of the process. A known or remembered nonce is
 * re-verified before it is used: a stale one costs one extra hash and a
 * search, never an invalid block. */
inline uint32_t MineLegacyProofOfWork(CBlockHeader header)
{
    static Mutex mutex;
    static std::map<uint256, uint32_t> found GUARDED_BY(mutex){KNOWN_LEGACY_NONCES.begin(), KNOWN_LEGACY_NONCES.end()};
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
