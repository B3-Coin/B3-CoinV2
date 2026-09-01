// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_MODERN_FINALITY_TYPES_H
#define B3COIN_MODERN_FINALITY_TYPES_H

#include <consensus/merkle.h>
#include <crypto/common.h>
#include <crypto/keccak256.h>
#include <hash.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace modern {

/**
 * B3 Cross-Chain Finality Protocol V1 — frozen types, layouts and digests
 * (doc/design/b3-cross-chain-finality-v1.md, owner rulings 2026-08-23).
 *
 * This header is the CODEC/TYPE layer (implementation plan, Commit 1):
 * fixed-width big-endian encodings, the tagged-hash digests Ethereum must be
 * able to recompute, the Keccak validator-set commitment, and the frozen
 * numeric constants. It performs no BLS cryptography, validation, or
 * activation itself; the implemented finality tracker, certificate verifier,
 * and F = M gates consume these types elsewhere.
 *
 * Layout rule: every integer is big-endian fixed width; no varints; one byte
 * representation per logical value (decoders reject any other length).
 */

// ---------------------------------------------------------------- constants

//! Validator-set commitment tree depth (8,192 slots) — frozen layout.
inline constexpr unsigned FINALITY_SET_TREE_DEPTH{13};
inline constexpr size_t MAX_FINALITY_SET{1u << FINALITY_SET_TREE_DEPTH};
//! Quorum ruleset identifier carried in every set header.
inline constexpr uint16_t FINALITY_RULESET_V1{1};
//! Weight unit: validator weights are whole modern B3 (1 modern B3 = 1 kB3 =
//! 1,000 legacy B3 = 10^9 base units, denomination ruling 2026-08-17);
//! w_i = floor(active_stake_base_units / FINALITY_WEIGHT_UNIT), w_i = 0 dropped.
inline constexpr int64_t FINALITY_WEIGHT_UNIT{1'000'000'000};

inline constexpr size_t BLS_PUBKEY_SIZE{48};
inline constexpr size_t BLS_SIGNATURE_SIZE{96};
inline constexpr size_t BIP340_SIG_SIZE{64};

//! MPA record maxima and declared verification costs (owner-frozen 2026-08-23).
//! 1 cost unit ~ 1 us on the reference machine (portable blst).
inline constexpr size_t FINALITY_CERTIFICATE_RECORD_MAX{112 + 1024 + 96}; // FinalizedBlock + bitmap(8192) + sig = 1,232
inline constexpr size_t FINALITY_KEY_EVIDENCE_SIZE{244};       // 32 + 48 + 4 + 64 + 96
inline constexpr int64_t FINALITY_CERTIFICATE_VERIFY_COST{2000};
inline constexpr int64_t FINALITY_KEY_EVIDENCE_VERIFY_COST{700};

//! The BLS scheme (curve, sizes, DSTs, decode and verification rules) is
//! frozen in crypto/bls.h — the only BLS surface consensus code may use.

//! Tagged-hash domains (BIP340 TaggedHash convention, SHA-256).
inline constexpr const char* FINALITY_DIGEST_TAG{"B3/FINALITY/V1"};
inline constexpr const char* FINALITY_BIND_TAG{"B3/FINALITY/BIND/V1"};
inline constexpr const char* FINALITY_CERT_COMMITMENT_TAG{"B3/FINALITY/CERT/V1"};

// ------------------------------------------------------------- fixed layouts

//! The object a finality certificate signs. 112 bytes (8+32+32+32+8).
struct FinalizedBlock {
    static constexpr size_t SIZE{8 + 32 + 32 + 32 + 8};
    uint64_t height{0};
    uint256 block_hash{};
    uint256 withdrawal_root{};     // all-zero before bridge activation
    uint256 validator_set_hash{};  // keccak(header of the SUCCESSOR set)
    uint64_t epoch{0};             // epoch of the signing set

    std::array<unsigned char, SIZE> Encode() const
    {
        std::array<unsigned char, SIZE> out{};
        WriteBE64(out.data(), height);
        std::copy(block_hash.begin(), block_hash.end(), out.data() + 8);
        std::copy(withdrawal_root.begin(), withdrawal_root.end(), out.data() + 40);
        std::copy(validator_set_hash.begin(), validator_set_hash.end(), out.data() + 72);
        WriteBE64(out.data() + 104, epoch);
        return out;
    }
    static std::optional<FinalizedBlock> Decode(std::span<const unsigned char> in)
    {
        if (in.size() != SIZE) return std::nullopt;
        FinalizedBlock fb;
        fb.height = ReadBE64(in.data());
        std::copy(in.begin() + 8, in.begin() + 40, fb.block_hash.begin());
        std::copy(in.begin() + 40, in.begin() + 72, fb.withdrawal_root.begin());
        std::copy(in.begin() + 72, in.begin() + 104, fb.validator_set_hash.begin());
        fb.epoch = ReadBE64(in.data() + 104);
        return fb;
    }
    friend bool operator==(const FinalizedBlock& a, const FinalizedBlock& b)
    {
        return a.height == b.height && a.block_hash == b.block_hash &&
               a.withdrawal_root == b.withdrawal_root &&
               a.validator_set_hash == b.validator_set_hash && a.epoch == b.epoch;
    }
};

//! Validator-set header. 110 bytes (8+2+4+8+8+48+32). validator_set_hash = keccak(Encode()).
struct ValidatorSetHeader {
    static constexpr size_t SIZE{8 + 2 + 4 + 8 + 8 + BLS_PUBKEY_SIZE + 32};
    uint64_t epoch{0};
    uint16_t ruleset_version{FINALITY_RULESET_V1};
    uint32_t validator_count{0};
    uint64_t total_weight{0};
    uint64_t quorum_weight{0};
    std::array<unsigned char, BLS_PUBKEY_SIZE> aggregate_pubkey{};
    uint256 members_root{};

    std::array<unsigned char, SIZE> Encode() const
    {
        std::array<unsigned char, SIZE> out{};
        WriteBE64(out.data(), epoch);
        WriteBE16(out.data() + 8, ruleset_version);
        WriteBE32(out.data() + 10, validator_count);
        WriteBE64(out.data() + 14, total_weight);
        WriteBE64(out.data() + 22, quorum_weight);
        std::copy(aggregate_pubkey.begin(), aggregate_pubkey.end(), out.data() + 30);
        std::copy(members_root.begin(), members_root.end(), out.data() + 78);
        return out;
    }
    static std::optional<ValidatorSetHeader> Decode(std::span<const unsigned char> in)
    {
        if (in.size() != SIZE) return std::nullopt;
        ValidatorSetHeader h;
        h.epoch = ReadBE64(in.data());
        h.ruleset_version = ReadBE16(in.data() + 8);
        h.validator_count = ReadBE32(in.data() + 10);
        h.total_weight = ReadBE64(in.data() + 14);
        h.quorum_weight = ReadBE64(in.data() + 22);
        std::copy(in.begin() + 30, in.begin() + 78, h.aggregate_pubkey.begin());
        std::copy(in.begin() + 78, in.begin() + 110, h.members_root.begin());
        return h;
    }
    friend bool operator==(const ValidatorSetHeader& a, const ValidatorSetHeader& b)
    {
        return a.Encode() == b.Encode();
    }
};

//! The certificate: signer bitmap (LSB-first within each byte, exactly
//! ceil(n/8) bytes, bits >= n zero) plus the 96-byte aggregate signature.
struct FinalityCertificate {
    std::vector<unsigned char> signer_bitmap;
    std::array<unsigned char, BLS_SIGNATURE_SIZE> aggregate_sig{};
};

//! FINALITY_KEY cell params: bls_pubkey || seq. 52 bytes (<= 80).
struct FinalityKeyParams {
    static constexpr size_t SIZE{BLS_PUBKEY_SIZE + 4};
    std::array<unsigned char, BLS_PUBKEY_SIZE> bls_pubkey{};
    uint32_t seq{0};

    std::array<unsigned char, SIZE> Encode() const
    {
        std::array<unsigned char, SIZE> out{};
        std::copy(bls_pubkey.begin(), bls_pubkey.end(), out.data());
        WriteBE32(out.data() + BLS_PUBKEY_SIZE, seq);
        return out;
    }
    static std::optional<FinalityKeyParams> Decode(std::span<const unsigned char> in)
    {
        if (in.size() != SIZE) return std::nullopt;
        FinalityKeyParams p;
        std::copy(in.begin(), in.begin() + BLS_PUBKEY_SIZE, p.bls_pubkey.begin());
        p.seq = ReadBE32(in.data() + BLS_PUBKEY_SIZE);
        return p;
    }
    //! The all-zero BLS pubkey is RESERVED for revocation.
    bool IsRevocation() const
    {
        for (unsigned char c : bls_pubkey) if (c != 0) return false;
        return true;
    }
};

//! FINALITY_CERT cell params: epoch || height. 16 bytes.
struct FinalityCertParams {
    static constexpr size_t SIZE{16};
    uint64_t epoch{0};
    uint64_t height{0};

    std::array<unsigned char, SIZE> Encode() const
    {
        std::array<unsigned char, SIZE> out{};
        WriteBE64(out.data(), epoch);
        WriteBE64(out.data() + 8, height);
        return out;
    }
    static std::optional<FinalityCertParams> Decode(std::span<const unsigned char> in)
    {
        if (in.size() != SIZE) return std::nullopt;
        return FinalityCertParams{ReadBE64(in.data()), ReadBE64(in.data() + 8)};
    }
};

//! FINALITY_KEY_EVIDENCE record payload. 244 bytes exactly.
struct FinalityKeyEvidence {
    static constexpr size_t SIZE{FINALITY_KEY_EVIDENCE_SIZE};
    std::array<unsigned char, 32> validator_key{};
    std::array<unsigned char, BLS_PUBKEY_SIZE> bls_pubkey{};
    uint32_t seq{0};
    std::array<unsigned char, BIP340_SIG_SIZE> bip340_sig{};
    std::array<unsigned char, BLS_SIGNATURE_SIZE> pop{};

    std::array<unsigned char, SIZE> Encode() const
    {
        std::array<unsigned char, SIZE> out{};
        size_t o{0};
        std::copy(validator_key.begin(), validator_key.end(), out.data() + o); o += 32;
        std::copy(bls_pubkey.begin(), bls_pubkey.end(), out.data() + o); o += BLS_PUBKEY_SIZE;
        WriteBE32(out.data() + o, seq); o += 4;
        std::copy(bip340_sig.begin(), bip340_sig.end(), out.data() + o); o += BIP340_SIG_SIZE;
        std::copy(pop.begin(), pop.end(), out.data() + o);
        return out;
    }
    static std::optional<FinalityKeyEvidence> Decode(std::span<const unsigned char> in)
    {
        if (in.size() != SIZE) return std::nullopt;
        FinalityKeyEvidence e;
        size_t o{0};
        std::copy(in.begin() + o, in.begin() + o + 32, e.validator_key.begin()); o += 32;
        std::copy(in.begin() + o, in.begin() + o + BLS_PUBKEY_SIZE, e.bls_pubkey.begin()); o += BLS_PUBKEY_SIZE;
        e.seq = ReadBE32(in.data() + o); o += 4;
        std::copy(in.begin() + o, in.begin() + o + BIP340_SIG_SIZE, e.bip340_sig.begin()); o += BIP340_SIG_SIZE;
        std::copy(in.begin() + o, in.begin() + o + BLS_SIGNATURE_SIZE, e.pop.begin());
        return e;
    }
};

// ------------------------------------------------------------------ bitmaps

//! Exact bitmap width for a set of n validators.
inline constexpr size_t SignerBitmapBytes(const size_t n) { return (n + 7) / 8; }

//! Bit i of the signer bitmap (LSB-first within each byte). Out-of-range bits read as 0.
inline bool SignerBit(std::span<const unsigned char> bitmap, const size_t i)
{
    if (i / 8 >= bitmap.size()) return false;
    return (bitmap[i / 8] >> (i % 8)) & 1;
}

//! Structural bitmap check: exactly ceil(n/8) bytes and every bit >= n is zero.
inline bool IsWellFormedSignerBitmap(std::span<const unsigned char> bitmap, const size_t n)
{
    if (n == 0 || n > MAX_FINALITY_SET) return false;
    if (bitmap.size() != SignerBitmapBytes(n)) return false;
    if (n % 8 != 0 && (bitmap.back() >> (n % 8)) != 0) return false;
    return true;
}

//! Certificate payload = FinalizedBlock(112) || bitmap(ceil(n/8)) || sig(96).
inline std::vector<unsigned char> EncodeCertificatePayload(const FinalizedBlock& fb, const FinalityCertificate& cert)
{
    std::vector<unsigned char> out;
    const auto fbb{fb.Encode()};
    out.insert(out.end(), fbb.begin(), fbb.end());
    out.insert(out.end(), cert.signer_bitmap.begin(), cert.signer_bitmap.end());
    out.insert(out.end(), cert.aggregate_sig.begin(), cert.aggregate_sig.end());
    return out;
}

//! Decode a certificate payload for a set of exactly n validators; any other
//! length or a malformed bitmap is rejected.
inline std::optional<std::pair<FinalizedBlock, FinalityCertificate>>
DecodeCertificatePayload(std::span<const unsigned char> in, const size_t n)
{
    if (n == 0 || n > MAX_FINALITY_SET) return std::nullopt;
    const size_t bm{SignerBitmapBytes(n)};
    if (in.size() != FinalizedBlock::SIZE + bm + BLS_SIGNATURE_SIZE) return std::nullopt;
    if (in.size() > FINALITY_CERTIFICATE_RECORD_MAX) return std::nullopt;
    const auto fb{FinalizedBlock::Decode(in.first(FinalizedBlock::SIZE))};
    if (!fb) return std::nullopt;
    FinalityCertificate cert;
    cert.signer_bitmap.assign(in.begin() + FinalizedBlock::SIZE, in.begin() + FinalizedBlock::SIZE + bm);
    if (!IsWellFormedSignerBitmap(cert.signer_bitmap, n)) return std::nullopt;
    std::copy(in.end() - BLS_SIGNATURE_SIZE, in.end(), cert.aggregate_sig.begin());
    return std::make_pair(*fb, std::move(cert));
}

// ------------------------------------------------------------------ digests

//! The message every validator BLS-signs: TaggedHash("B3/FINALITY/V1", domain || FinalizedBlock).
inline uint256 FinalityDigest(const uint256& chain_domain, const FinalizedBlock& fb)
{
    HashWriter w{TaggedHash(FINALITY_DIGEST_TAG)};
    w << std::span<const unsigned char>(chain_domain.begin(), 32);
    w << std::span<const unsigned char>(fb.Encode());
    return w.GetSHA256();
}

//! The message the BIP340 identity key signs to authorize a BLS binding.
inline uint256 FinalityBindDigest(const uint256& chain_domain, std::span<const unsigned char> validator_key,
                                  std::span<const unsigned char> bls_pubkey, const uint32_t seq)
{
    HashWriter w{TaggedHash(FINALITY_BIND_TAG)};
    w << std::span<const unsigned char>(chain_domain.begin(), 32);
    w << validator_key << bls_pubkey;
    unsigned char s[4];
    WriteBE32(s, seq);
    w << std::span<const unsigned char>(s, 4);
    return w.GetSHA256();
}

//! FINALITY_CERT cell commitment = TaggedHash("B3/FINALITY/CERT/V1", certificate payload).
inline uint256 FinalityCertCommitment(std::span<const unsigned char> payload)
{
    HashWriter w{TaggedHash(FINALITY_CERT_COMMITMENT_TAG)};
    w << payload;
    return w.GetSHA256();
}

// -------------------------------------------------- quorum and set commitment

//! Ruleset 1: floor(2W/3) + 1, stake-weighted, whole modern B3 units.
inline constexpr uint64_t QuorumWeightV1(const uint64_t total_weight)
{
    // 2*W cannot overflow for any realistic W; guard anyway.
    const uint64_t twice{total_weight > (UINT64_MAX / 2) ? UINT64_MAX : total_weight * 2};
    return twice / 3 + 1;
}

inline uint256 Keccak(std::span<const unsigned char> data)
{
    uint256 out;
    Keccak256().Write(data).Finalize(out);
    return out;
}

//! leaf_i = keccak(u32 i || bls_pubkey_i(48) || u64 w_i); i = 0..n-1.
inline uint256 ValidatorSetLeaf(const uint32_t index, std::span<const unsigned char> bls_pubkey, const uint64_t weight)
{
    unsigned char pre[4 + BLS_PUBKEY_SIZE + 8];
    WriteBE32(pre, index);
    std::copy(bls_pubkey.begin(), bls_pubkey.begin() + BLS_PUBKEY_SIZE, pre + 4);
    WriteBE64(pre + 4 + BLS_PUBKEY_SIZE, weight);
    return Keccak(pre);
}

//! members_root: complete binary keccak tree over 2^13 leaves, zero-padded.
//! Returns nullopt if more than MAX_FINALITY_SET leaves are supplied.
inline std::optional<uint256> ValidatorSetMembersRoot(std::vector<uint256> leaves)
{
    if (leaves.size() > MAX_FINALITY_SET) return std::nullopt;
    leaves.resize(MAX_FINALITY_SET, uint256{}); // padding leaves are 32 zero bytes
    std::vector<uint256> level{std::move(leaves)};
    while (level.size() > 1) {
        std::vector<uint256> next;
        next.reserve(level.size() / 2);
        for (size_t i = 0; i < level.size(); i += 2) {
            unsigned char pre[64];
            std::copy(level[i].begin(), level[i].end(), pre);
            std::copy(level[i + 1].begin(), level[i + 1].end(), pre + 32);
            next.push_back(Keccak(pre));
        }
        level = std::move(next);
    }
    return level[0];
}

//! validator_set_hash = keccak(header bytes).
inline uint256 ValidatorSetHash(const ValidatorSetHeader& header)
{
    return Keccak(header.Encode());
}

} // namespace modern

#endif // B3COIN_MODERN_FINALITY_TYPES_H
