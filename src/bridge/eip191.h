// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_BRIDGE_EIP191_H
#define B3COIN_BRIDGE_EIP191_H

#include <consensus/bridge_params.h>
#include <crypto/keccak256.h>
#include <pubkey.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * EIP-191 personal_sign proof of a bridge V2 refund EOA (contracts spec
 * section 7.3; node changes section 11.2). Wallet policy only, never
 * consensus: a route (b) deposit forwarder pays `refundEOA` on refund and
 * returnToOwner, so the wallet derives F_d only for an Ethereum EOA whose key
 * signed
 *
 *   B3 bridge V2 refund EOA\n
 *   chain domain: 0x<64 hex of the raw chain-domain bytes>\n
 *   refund EOA: 0x<40 lowercase hex>
 *
 * (no trailing newline). Binding the chain domain keeps a regtest or testnet
 * proof from serving mainnet. A Safe or an ERC-4337 account cannot produce
 * the signature, which is the point. Shared vectors:
 * contracts/test/vectors/bridge_v2_vectors.json "forwarder_v2.refund_eoa_message".
 */
namespace bridge {

inline constexpr std::string_view REFUND_EOA_MESSAGE_PREFIX_V2{"B3 bridge V2 refund EOA"};
//! r || s || v, as eth_sign / personal_sign return it.
inline constexpr size_t EIP191_SIGNATURE_SIZE{65};

/** The exact text the refund EOA signs. */
inline std::string RefundEoaMessageV2(const uint256& chain_domain,
                                      const Consensus::BridgeEthAddress& eoa)
{
    std::string out{REFUND_EOA_MESSAGE_PREFIX_V2};
    out += "\nchain domain: 0x";
    out += HexStr(std::span<const unsigned char>{chain_domain.begin(), chain_domain.end()});
    out += "\nrefund EOA: 0x";
    out += HexStr(eoa);
    return out;
}

/** keccak256("\x19Ethereum Signed Message:\n" || decimal(len) || message). */
inline uint256 Eip191Digest(const std::string_view message)
{
    const std::string prefix{"\x19" "Ethereum Signed Message:\n" + std::to_string(message.size())};
    uint256 out;
    Keccak256()
        .Write(std::span<const unsigned char>{
            reinterpret_cast<const unsigned char*>(prefix.data()), prefix.size()})
        .Write(std::span<const unsigned char>{
            reinterpret_cast<const unsigned char*>(message.data()), message.size()})
        .Finalize(out);
    return out;
}

/** keccak256(X || Y)[12:] of a valid public key; nullopt for an invalid one. */
inline std::optional<Consensus::BridgeEthAddress> EthAddressFromPubKey(CPubKey pubkey)
{
    if (!pubkey.IsFullyValid()) return std::nullopt;
    if (pubkey.IsCompressed() && !pubkey.Decompress()) return std::nullopt;
    if (pubkey.size() != CPubKey::SIZE || pubkey[0] != 0x04) return std::nullopt;
    uint256 hash;
    Keccak256()
        .Write(std::span<const unsigned char>{pubkey.data() + 1, pubkey.size() - 1})
        .Finalize(hash);
    Consensus::BridgeEthAddress out{};
    std::copy(hash.begin() + 12, hash.end(), out.begin());
    return out;
}

/**
 * The address whose key produced the 65-byte r || s || v signature over the
 * EIP-191 digest of `message`, as ecrecover returns it. v is 27 or 28 (or the
 * raw recovery id 0 or 1); anything else, a wrong length or a failed
 * recovery yields nullopt. A high-s signature is accepted, as ecrecover
 * does: the signature proves key control, and its malleability is harmless
 * here.
 */
inline std::optional<Consensus::BridgeEthAddress> RecoverEip191SignerV2(
    const std::string_view message, const std::span<const unsigned char> signature)
{
    if (signature.size() != EIP191_SIGNATURE_SIZE) return std::nullopt;
    const uint8_t v{signature[64]};
    uint8_t recid;
    if (v == 27 || v == 28) {
        recid = v - 27;
    } else if (v == 0 || v == 1) {
        recid = v;
    } else {
        return std::nullopt;
    }
    // Bitcoin's compact layout: header 27 + recid (uncompressed key), r, s.
    std::vector<unsigned char> compact(CPubKey::COMPACT_SIGNATURE_SIZE);
    compact[0] = static_cast<unsigned char>(27 + recid);
    std::copy(signature.begin(), signature.begin() + 64, compact.begin() + 1);
    CPubKey pubkey;
    if (!pubkey.RecoverCompact(Eip191Digest(message), compact)) return std::nullopt;
    return EthAddressFromPubKey(pubkey);
}

} // namespace bridge

#endif // B3COIN_BRIDGE_EIP191_H
