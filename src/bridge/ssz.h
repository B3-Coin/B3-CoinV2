// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_BRIDGE_SSZ_H
#define B3COIN_BRIDGE_SSZ_H

#include <crypto/common.h>
#include <crypto/sha256.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

/** Minimal SSZ hash-tree-root machinery for the Ethereum beacon light client
 *  used by the independently gated type-10 consensus bridge.
 *
 *  Only what the sync-committee light client needs is implemented:
 *  fixed-size containers, Bytes48/Bytes4/uint64/uint256 leaves, the two
 *  byte-blob shapes in ExecutionPayloadHeader (ByteVector[256] logs_bloom,
 *  ByteList[32] extra_data), vectors of Bytes48 (committee pubkeys), and
 *  generalized-index Merkle branch verification.
 */
namespace bridge {
namespace ssz {

inline uint256 HashPair(const uint256& a, const uint256& b)
{
    uint256 out;
    CSHA256().Write(a.begin(), 32).Write(b.begin(), 32).Finalize(out.begin());
    return out;
}

//! Root of an all-zero subtree of the given depth.
inline uint256 ZeroSubtree(unsigned depth)
{
    uint256 z{};
    for (unsigned i = 0; i < depth; ++i) z = HashPair(z, z);
    return z;
}

//! Merkleize chunks into a tree with 2^depth leaves (zero-chunk padded).
inline uint256 Merkleize(std::vector<uint256> chunks, unsigned depth)
{
    if (chunks.empty()) return ZeroSubtree(depth);
    if (chunks.size() > (size_t{1} << depth)) return uint256{}; // caller bug; roots never all-zero-collide in practice
    uint256 zero{}; // zero-subtree hash at the current level
    for (unsigned level = 0; level < depth; ++level) {
        const size_t n{chunks.size()};
        std::vector<uint256> next((n + 1) / 2);
        for (size_t i = 0; i < n; i += 2) {
            next[i / 2] = HashPair(chunks[i], i + 1 < n ? chunks[i + 1] : zero);
        }
        chunks = std::move(next);
        zero = HashPair(zero, zero);
    }
    return chunks[0];
}

inline uint256 LeafUint64(uint64_t v)
{
    uint256 out{};
    WriteLE64(out.begin(), v);
    return out;
}

//! Bytes up to 32, left-aligned zero-padded chunk.
inline uint256 LeafBytes(std::span<const unsigned char> b)
{
    // This helper is used at the bottom of consensus-facing SSZ hash trees.
    // Never let a malformed variable-size field turn an ordinary validation
    // failure into an out-of-bounds write.
    if (b.size() > 32) return uint256{};
    uint256 out{};
    std::copy(b.begin(), b.end(), out.begin());
    return out;
}

//! Bytes48 (BLS pubkey / any 33..64-byte value): two chunks.
inline uint256 RootBytes48(std::span<const unsigned char> b48)
{
    uint256 c0{}, c1{};
    std::copy(b48.begin(), b48.begin() + 32, c0.begin());
    std::copy(b48.begin() + 32, b48.end(), c1.begin());
    return HashPair(c0, c1);
}

//! ByteVector[N] for N a multiple of 32 (logs_bloom: N=256 -> 8 chunks).
inline uint256 RootByteVector(std::span<const unsigned char> data, unsigned depth)
{
    std::vector<uint256> chunks;
    for (size_t i = 0; i < data.size(); i += 32) {
        chunks.push_back(LeafBytes(data.subspan(i, std::min<size_t>(32, data.size() - i))));
    }
    return Merkleize(std::move(chunks), depth);
}

inline uint256 MixInLength(const uint256& root, uint64_t len)
{
    return HashPair(root, LeafUint64(len));
}

//! ByteList[max<=32] (extra_data): one chunk limit, length mixed in.
inline uint256 RootByteList32(std::span<const unsigned char> data)
{
    if (data.size() > 32) return uint256{};
    return MixInLength(data.empty() ? uint256{} : LeafBytes(data), data.size());
}

//! Vector[Bytes48, N]: element roots merkleized to the exact depth.
inline uint256 RootPubkeyVector(std::span<const std::array<unsigned char, 48>> keys, unsigned depth)
{
    std::vector<uint256> roots;
    roots.reserve(keys.size());
    for (const auto& k : keys) roots.push_back(RootBytes48(k));
    return Merkleize(std::move(roots), depth);
}

//! Container: merkleize the field roots at the minimal power-of-two width.
inline uint256 RootContainer(std::vector<uint256> fields)
{
    unsigned depth{0};
    while ((size_t{1} << depth) < fields.size()) ++depth;
    return Merkleize(std::move(fields), depth);
}

/** is_valid_merkle_branch, addressed by generalized index:
 *  depth = floor(log2(gindex)), position = gindex - 2^depth. */
inline bool VerifyBranch(const uint256& leaf, std::span<const uint256> branch,
                         uint64_t gindex, const uint256& root)
{
    if (gindex < 2) return false;
    unsigned depth{0};
    while ((uint64_t{2} << depth) <= gindex) ++depth; // depth = floor(log2)
    if (branch.size() != depth) return false;
    uint64_t index{gindex - (uint64_t{1} << depth)};
    uint256 node{leaf};
    for (unsigned k = 0; k < depth; ++k) {
        node = ((index >> k) & 1) ? HashPair(branch[k], node) : HashPair(node, branch[k]);
    }
    return node == root;
}

// ---------------------------------------------------------------------------
// Beacon containers (Altair..Electra field layouts used by the light client).

struct BeaconBlockHeader {
    uint64_t slot{0};
    uint64_t proposer_index{0};
    uint256 parent_root{};
    uint256 state_root{};
    uint256 body_root{};

    uint256 HashTreeRoot() const
    {
        return RootContainer({LeafUint64(slot), LeafUint64(proposer_index),
                              parent_root, state_root, body_root});
    }

    friend bool operator==(const BeaconBlockHeader&,
                           const BeaconBlockHeader&) = default;
};

//! ExecutionPayloadHeader (Deneb/Electra shape: 17 fields).
struct ExecutionPayloadHeader {
    uint256 parent_hash{};
    std::array<unsigned char, 20> fee_recipient{};
    uint256 state_root{};
    uint256 receipts_root{};
    std::array<unsigned char, 256> logs_bloom{};
    uint256 prev_randao{};
    uint64_t block_number{0};
    uint64_t gas_limit{0};
    uint64_t gas_used{0};
    uint64_t timestamp{0};
    std::vector<unsigned char> extra_data{}; // ByteList[32]
    uint256 base_fee_per_gas{};              // uint256, little-endian chunk
    uint256 block_hash{};
    uint256 transactions_root{};
    uint256 withdrawals_root{};
    uint64_t blob_gas_used{0};
    uint64_t excess_blob_gas{0};

    bool ValidForHashTreeRoot() const { return extra_data.size() <= 32; }

    uint256 HashTreeRoot() const
    {
        if (!ValidForHashTreeRoot()) return uint256{};
        return RootContainer({parent_hash,
                              LeafBytes(fee_recipient),
                              state_root,
                              receipts_root,
                              RootByteVector(logs_bloom, 3),
                              prev_randao,
                              LeafUint64(block_number),
                              LeafUint64(gas_limit),
                              LeafUint64(gas_used),
                              LeafUint64(timestamp),
                              RootByteList32(extra_data),
                              base_fee_per_gas,
                              block_hash,
                              transactions_root,
                              withdrawals_root,
                              LeafUint64(blob_gas_used),
                              LeafUint64(excess_blob_gas)});
    }

    friend bool operator==(const ExecutionPayloadHeader&,
                           const ExecutionPayloadHeader&) = default;
};

static constexpr size_t SYNC_COMMITTEE_SIZE{512};

struct SyncCommittee {
    std::vector<std::array<unsigned char, 48>> pubkeys{}; // exactly 512
    std::array<unsigned char, 48> aggregate_pubkey{};

    uint256 HashTreeRoot() const
    {
        return RootContainer({RootPubkeyVector(pubkeys, 9), RootBytes48(aggregate_pubkey)});
    }

    friend bool operator==(const SyncCommittee&, const SyncCommittee&) = default;
};

//! compute_domain + compute_signing_root (DOMAIN_SYNC_COMMITTEE = 0x07000000).
inline uint256 ForkDataRoot(std::span<const unsigned char> fork_version4,
                            const uint256& genesis_validators_root)
{
    return RootContainer({LeafBytes(fork_version4), genesis_validators_root});
}

inline std::array<unsigned char, 32> SyncCommitteeDomain(std::span<const unsigned char> fork_version4,
                                                         const uint256& genesis_validators_root)
{
    const uint256 fdr{ForkDataRoot(fork_version4, genesis_validators_root)};
    std::array<unsigned char, 32> domain{};
    domain[0] = 0x07; // DOMAIN_SYNC_COMMITTEE
    std::copy(fdr.begin(), fdr.begin() + 28, domain.begin() + 4);
    return domain;
}

inline uint256 SigningRoot(const uint256& object_root, std::span<const unsigned char> domain32)
{
    return RootContainer({object_root, LeafBytes(domain32)});
}

} // namespace ssz
} // namespace bridge

#endif // B3COIN_BRIDGE_SSZ_H
