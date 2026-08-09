// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit-license.php.

#ifndef B3COIN_CONSENSUS_BLOCK_CODEC_H
#define B3COIN_CONSENSUS_BLOCK_CODEC_H

#include <consensus/era.h>

#include <cstdint>

namespace Consensus {

/**
 * B3's permanent post-fork block-body codec marker.
 *
 * BIP9 reserves the three most-significant header-version bits for the
 * versionbits namespace (001, or 0x20000000). Bit 27 is permanently reserved
 * by B3, while bit 28 remains available for Core's testdummy deployment. A
 * modern B3 header therefore has at least version 0x28000000, serialized as
 * the four little-endian bytes 00 00 00 28. Lower versionbits remain available
 * for future B3 deployments.
 *
 * This is not a versionbits deployment or a miner signal. It is a permanent
 * consensus marker for choosing the raw block-body codec: legacy B3 blocks
 * carry transaction nTime values and a trailing block signature; modern
 * blocks use the unmodified Core encoding.
 */
inline constexpr uint32_t B3_BLOCK_CODEC_V2_BIT{0x08000000U};
inline constexpr uint32_t B3_VERSIONBITS_TOP_BITS{0x20000000U};
inline constexpr uint32_t B3_VERSIONBITS_TOP_MASK{0xE0000000U};
inline constexpr uint32_t B3_BLOCK_CODEC_V2_MASK{B3_VERSIONBITS_TOP_MASK | B3_BLOCK_CODEC_V2_BIT};
inline constexpr uint32_t B3_BLOCK_CODEC_V2_VERSION{B3_VERSIONBITS_TOP_BITS | B3_BLOCK_CODEC_V2_BIT};

/** True when a raw header explicitly selects the modern B3 block-body codec. */
constexpr bool HasB3BlockCodecV2(const int32_t n_version)
{
    return (static_cast<uint32_t>(n_version) & B3_BLOCK_CODEC_V2_MASK) ==
           B3_BLOCK_CODEC_V2_VERSION;
}

/**
 * Add the immutable B3 codec marker while retaining future low versionbits.
 * Call this only when constructing a known post-fork B3 block.
 */
constexpr int32_t WithB3BlockCodecV2(const int32_t n_version)
{
    const uint32_t version{static_cast<uint32_t>(n_version)};
    return static_cast<int32_t>((version & ~B3_VERSIONBITS_TOP_MASK) |
                                B3_BLOCK_CODEC_V2_VERSION);
}

/**
 * The marker is an additional B3 consensus invariant only after a boundary
 * has been configured. Non-B3 chains retain ordinary Bitcoin Core version
 * semantics. The height is authoritative; the header marker is the matching
 * raw-codec selector and must agree with that height.
 */
constexpr bool HasExpectedB3BlockCodec(const int32_t n_version, const int height,
                                       const Params& params)
{
    if (!params.legacy_b3coin) return true;
    const bool modern_era{GetB3Era(height, params) == B3Era::MODERN};
    return HasB3BlockCodecV2(n_version) == modern_era;
}

} // namespace Consensus

#endif // B3COIN_CONSENSUS_BLOCK_CODEC_H
