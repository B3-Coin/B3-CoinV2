// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_BRIDGE_BOOTSTRAP_V2_H
#define B3COIN_BRIDGE_BOOTSTRAP_V2_H

#include <bridge/deposit.h>
#include <crypto/common.h>
#include <hash.h>
#include <modern/finality_v2.h>
#include <uint256.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * The bridge V2 K_OF_N bootstrap statement (open issue OI-12, owner ruling R9
 * of 2026-10-05; doc/design/b3-bridge-v2-contracts-spec.md section 5 step 3).
 * contracts/src/v2/B3TypesV2.sol bootstrapMessage/bootstrapDigest own the
 * bytes; this header mirrors them exactly:
 *
 *   message = chainDomain(32) || u256 evmChainId (big-endian, 32)
 *             || verifier(20) || startSetHash(32) || FinalizedBlockV2(185)
 *                                                              (301 bytes)
 *   digest  = TaggedHash("B3/BRIDGE/BOOTSTRAP/V2", message)
 *
 * Hashes keep their raw uint256 byte order, exactly as FinalityDigestV2 does,
 * so the 32 bytes at an offset equal the Solidity bytes32 value. The four
 * bootstrap keys BLS-sign `digest`, which binds one verifier deployment on one
 * EVM chain and its epoch-k start set, so an attestation can never initialize
 * another deployment. Its tag differs from digest V2's, so an attestation and
 * a committee certificate never stand in for each other.
 *
 * Pure: no I/O, no node types, not height-gated. B3 consensus never verifies
 * this message; it is shared by tools and the cross-language vector tests.
 */
namespace bridge {

inline constexpr std::string_view BRIDGE_BOOTSTRAP_V2_TAG{"B3/BRIDGE/BOOTSTRAP/V2"};
inline constexpr size_t BRIDGE_BOOTSTRAP_V2_MESSAGE_SIZE{32 + 32 + 20 + 32 + modern::FinalizedBlockV2::SIZE};
static_assert(BRIDGE_BOOTSTRAP_V2_MESSAGE_SIZE == 301);

//! The 301-byte K_OF_N bootstrap statement B3TypesV2.bootstrapMessage encodes.
inline std::vector<unsigned char> BootstrapAttestationMessageV2(const uint256& chain_domain,
                                                                const uint64_t evm_chain_id,
                                                                const EthAddress& verifier,
                                                                const uint256& start_set_hash,
                                                                const modern::FinalizedBlockV2& fb)
{
    std::vector<unsigned char> out(BRIDGE_BOOTSTRAP_V2_MESSAGE_SIZE, 0);
    unsigned char* p{out.data()};
    p = std::copy(chain_domain.begin(), chain_domain.end(), p);
    // uint256 block.chainid: 24 zero bytes, then the big-endian 64-bit value.
    p += 24;
    WriteBE64(p, evm_chain_id);
    p += 8;
    p = std::copy(verifier.begin(), verifier.end(), p);
    p = std::copy(start_set_hash.begin(), start_set_hash.end(), p);
    const auto encoded{fb.Encode()};
    std::copy(encoded.begin(), encoded.end(), p);
    return out;
}

//! bootstrapDigestV2 = TaggedHash("B3/BRIDGE/BOOTSTRAP/V2", message): the BLS
//! message the K_OF_N bootstrap keys sign (B3TypesV2.bootstrapDigest).
inline uint256 BootstrapAttestationDigestV2(const uint256& chain_domain, const uint64_t evm_chain_id,
                                            const EthAddress& verifier, const uint256& start_set_hash,
                                            const modern::FinalizedBlockV2& fb)
{
    const auto message{BootstrapAttestationMessageV2(chain_domain, evm_chain_id, verifier, start_set_hash, fb)};
    HashWriter w{TaggedHash(std::string{BRIDGE_BOOTSTRAP_V2_TAG})};
    w << std::span<const unsigned char>(message);
    return w.GetSHA256();
}

} // namespace bridge

#endif // B3COIN_BRIDGE_BOOTSTRAP_V2_H
