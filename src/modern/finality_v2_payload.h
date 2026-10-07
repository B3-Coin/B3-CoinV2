// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_MODERN_FINALITY_V2_PAYLOAD_H
#define B3COIN_MODERN_FINALITY_V2_PAYLOAD_H

#include <crypto/common.h>
#include <modern/finality_types.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace modern {

/**
 * Fresh finality lineage, digest V2: the fixed FinalizedBlockV2 layout and
 * the certificate payload V2 codec, split out of modern/finality_v2.h (which
 * includes this header and documents the bytes) so that bridge/proof.h can
 * decode a kind 13 FINALITY_CONFLICT record without an include cycle:
 * finality_v2.h reaches modern/mpa.h through finality_certificate.h, and
 * mpa.h includes bridge/proof.h. This header depends only on
 * modern/finality_types.h and must stay that way.
 */

//! bridgeFlags bit 0: B3 closed burns for good; the verifier closes deposits.
inline constexpr uint8_t FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED{0x01};
//! Every bit a decoder accepts; all other bits must be zero.
inline constexpr uint8_t FINALITY_BRIDGE_FLAGS_KNOWN{FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED};

// ------------------------------------------------------------- fixed layout

//! The object a fresh-lineage certificate signs. 185 bytes.
struct FinalizedBlockV2 {
    static constexpr size_t SIZE{32 + 8 + 32 + 32 + 32 + 8 + 8 + 32 + 1};
    uint256 lineage_id{};
    uint64_t height{0};
    uint256 block_hash{};
    uint256 withdrawal_root{};         // zero below BRIDGE_ACTIVATION_HEIGHT
    uint256 validator_set_hash{};      // keccak(ruleset-2 header of the SUCCESSOR set)
    uint64_t epoch{0};                 // fresh-lineage epoch of the signing set
    uint64_t checkpoint_index{0};      // (height - GENESIS_HEIGHT) / interval, >= 1 when certified
    uint256 checkpoint_history_root{}; // root over history leaves 0 .. checkpoint_index-1
    uint8_t bridge_flags{0};           // bit 0 only

    std::array<unsigned char, SIZE> Encode() const
    {
        std::array<unsigned char, SIZE> out{};
        unsigned char* p{out.data()};
        p = std::copy(lineage_id.begin(), lineage_id.end(), p);
        WriteBE64(p, height); p += 8;
        p = std::copy(block_hash.begin(), block_hash.end(), p);
        p = std::copy(withdrawal_root.begin(), withdrawal_root.end(), p);
        p = std::copy(validator_set_hash.begin(), validator_set_hash.end(), p);
        WriteBE64(p, epoch); p += 8;
        WriteBE64(p, checkpoint_index); p += 8;
        p = std::copy(checkpoint_history_root.begin(), checkpoint_history_root.end(), p);
        *p = bridge_flags;
        return out;
    }
    //! Exactly 185 bytes; any bridge-flag bit other than bit 0 is rejected.
    static std::optional<FinalizedBlockV2> Decode(std::span<const unsigned char> in)
    {
        if (in.size() != SIZE) return std::nullopt;
        if ((in[SIZE - 1] & ~FINALITY_BRIDGE_FLAGS_KNOWN) != 0) return std::nullopt;
        FinalizedBlockV2 fb;
        const unsigned char* p{in.data()};
        auto read_hash{[&p](uint256& h) { std::copy(p, p + 32, h.begin()); p += 32; }};
        read_hash(fb.lineage_id);
        fb.height = ReadBE64(p); p += 8;
        read_hash(fb.block_hash);
        read_hash(fb.withdrawal_root);
        read_hash(fb.validator_set_hash);
        fb.epoch = ReadBE64(p); p += 8;
        fb.checkpoint_index = ReadBE64(p); p += 8;
        read_hash(fb.checkpoint_history_root);
        fb.bridge_flags = *p;
        return fb;
    }
    friend bool operator==(const FinalizedBlockV2& a, const FinalizedBlockV2& b)
    {
        return a.Encode() == b.Encode();
    }
};
static_assert(FinalizedBlockV2::SIZE == 185);
static_assert(FINALITY_CERTIFICATE_V2_RECORD_MAX ==
              FinalizedBlockV2::SIZE + MAX_FINALITY_SET / 8 + BLS_SIGNATURE_SIZE);
static_assert(FINALITY_CERTIFICATE_V2_RECORD_MIN == FinalizedBlockV2::SIZE + 1 + BLS_SIGNATURE_SIZE);

// ------------------------------------------------------------------ payload

//! Certificate payload V2 = FinalizedBlockV2(185) || bitmap(ceil(n/8)) || sig(96).
inline std::vector<unsigned char> EncodeCertificatePayloadV2(const FinalizedBlockV2& fb, const FinalityCertificate& cert)
{
    std::vector<unsigned char> out;
    const auto fbb{fb.Encode()};
    out.reserve(fbb.size() + cert.signer_bitmap.size() + cert.aggregate_sig.size());
    out.insert(out.end(), fbb.begin(), fbb.end());
    out.insert(out.end(), cert.signer_bitmap.begin(), cert.signer_bitmap.end());
    out.insert(out.end(), cert.aggregate_sig.begin(), cert.aggregate_sig.end());
    return out;
}

//! Decode a V2 payload for a set of exactly n validators; any other length,
//! a malformed bitmap or an unknown bridge-flag bit is rejected.
inline std::optional<std::pair<FinalizedBlockV2, FinalityCertificate>>
DecodeCertificatePayloadV2(std::span<const unsigned char> in, const size_t n)
{
    if (n == 0 || n > MAX_FINALITY_SET) return std::nullopt;
    const size_t bm{SignerBitmapBytes(n)};
    if (in.size() != FinalizedBlockV2::SIZE + bm + BLS_SIGNATURE_SIZE) return std::nullopt;
    if (in.size() > FINALITY_CERTIFICATE_V2_RECORD_MAX) return std::nullopt;
    const auto fb{FinalizedBlockV2::Decode(in.first(FinalizedBlockV2::SIZE))};
    if (!fb) return std::nullopt;
    FinalityCertificate cert;
    cert.signer_bitmap.assign(in.begin() + FinalizedBlockV2::SIZE, in.begin() + FinalizedBlockV2::SIZE + bm);
    if (!IsWellFormedSignerBitmap(cert.signer_bitmap, n)) return std::nullopt;
    std::copy(in.end() - BLS_SIGNATURE_SIZE, in.end(), cert.aggregate_sig.begin());
    return std::make_pair(*fb, std::move(cert));
}

} // namespace modern

#endif // B3COIN_MODERN_FINALITY_V2_PAYLOAD_H
