// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_FN_GENESIS_H
#define B3COIN_MODERN_FN_GENESIS_H

#include <consensus/amount.h>
#include <consensus/fn_params.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <span.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace modern {

// ---- Frozen historical FN designation rule -----------------------------

//! Exactly 1 old-B3 in the legacy base-unit convention. The historical
//! client registered an FN identity only when the disintegration transaction
//! itself contained an output of this value in byte-exact P2PKH form.
inline constexpr CAmount LEGACY_FN_MARKER_VALUE{1'000'000};

//! Byte-exact legacy P2PKH matcher. This interprets sealed historical bytes;
//! it is not a modern output policy or a standardness rule.
inline bool IsLegacyFnP2pkh(const CScript& script)
{
    return script.size() == 25 && script[0] == OP_DUP &&
           script[1] == OP_HASH160 && script[2] == 0x14 &&
           script[23] == OP_EQUALVERIFY && script[24] == OP_CHECKSIG;
}

/**
 * Return the lowest-index historical FN designation in a disintegration.
 * A qualifying value-gap event with no exact designation is ignored: no
 * fallback recipient is derived and later spending does not change the row.
 */
inline std::optional<uint32_t> FindLegacyFnRecipientVout(const CTransaction& tx)
{
    for (uint32_t n{0}; n < tx.vout.size(); ++n) {
        if (tx.vout[n].nValue == LEGACY_FN_MARKER_VALUE &&
            IsLegacyFnP2pkh(tx.vout[n].scriptPubKey)) {
            return n;
        }
    }
    return std::nullopt;
}

/**
 * FN Genesis v1 manifest commitment.
 *
 * A manifest is a non-empty sequence of Consensus::FnGenesisRight rows,
 * strictly increasing by the 32 raw bytes of `pod_id`. The comparison is on
 * uint256's internal/serialized byte order, deliberately not the reversed
 * human-facing order returned by GetHex(). Consequently every PoD appears at
 * most once, while two different PoDs may designate the same recipient.
 *
 * All integer fields below are unsigned BIG-ENDIAN. Hash fields use their raw
 * internal/serialization bytes. The construction is:
 *
 * FN Genesis occurs in the mandatory first corridor block H+1 (the hard-fork
 * height). `fn_genesis_height` below is that height, not a later modern-era
 * activation gate.
 *
 *   context = chain_domain[32] || fn_genesis_height:u32 ||
 *             manifest_version:u16 || row_count:u32
 *
 *   leaf[i] = TaggedHash("B3/FN/GENESIS/LEAF/V1",
 *                        context || row_index:u32 || pod_id[32] ||
 *                        recipient_key_hash[20])
 *
 *   tree     = a binary Merkle tree over leaf[0..count-1], duplicating the
 *              final hash at an odd level, with each parent equal to
 *              TaggedHash("B3/FN/GENESIS/NODE/V1", left[32] || right[32])
 *
 *   root     = TaggedHash("B3/FN/GENESIS/ROOT/V1", context || tree[32])
 *
 * The positional leaf and committed count remove the duplicate-tail ambiguity
 * of an odd-leaf Merkle tree. Separate leaf/node/root tags prevent hashes from
 * one level being reinterpreted at another. The outer envelope makes every
 * manifest-level parameter visibly bound even for a one-row tree.
 */
inline constexpr uint16_t FN_GENESIS_MANIFEST_VERSION_V1{1};
inline constexpr const char* FN_GENESIS_LEAF_TAG{"B3/FN/GENESIS/LEAF/V1"};
inline constexpr const char* FN_GENESIS_NODE_TAG{"B3/FN/GENESIS/NODE/V1"};
inline constexpr const char* FN_GENESIS_ROOT_TAG{"B3/FN/GENESIS/ROOT/V1"};

/**
 * Canonical release artifact carrying the complete FN Genesis manifest.
 *
 * The byte layout is deliberately fixed-width and independent of host
 * endianness or locale:
 *
 *   "b3-fn-genesis/v1\n"[17] || chain_domain[32] ||
 *   fn_genesis_height:u32be || manifest_version:u16be || row_count:u32be ||
 *   rights_root[32] ||
 *   row[0].pod_id[32] || row[0].recipient_key_hash[20] || ...
 *
 * Hash fields are their raw internal/serialization bytes. The rows use the
 * exact ordering validated by ValidateFnGenesisManifest. The embedded root is
 * recomputed by the encoder, so a caller cannot export inconsistent context,
 * rows, and commitment. SHA256 of these complete bytes is the publication
 * checksum used to compare independent sealed-history runs.
 */
inline constexpr std::string_view FN_GENESIS_MANIFEST_FILE_MAGIC{
    "b3-fn-genesis/v1\n"};
static_assert(FN_GENESIS_MANIFEST_FILE_MAGIC.size() == 17);

namespace fn_genesis_detail {

inline constexpr size_t CONTEXT_SIZE{32 + 4 + 2 + 4};

inline std::array<unsigned char, CONTEXT_SIZE> EncodeContext(
    const uint256& chain_domain,
    const uint32_t fn_genesis_height,
    const uint16_t manifest_version,
    const uint32_t row_count)
{
    std::array<unsigned char, CONTEXT_SIZE> out{};
    std::copy(chain_domain.begin(), chain_domain.end(), out.begin());
    WriteBE32(out.data() + 32, fn_genesis_height);
    WriteBE16(out.data() + 36, manifest_version);
    WriteBE32(out.data() + 38, row_count);
    return out;
}

inline uint256 NodeHash(const uint256& left, const uint256& right)
{
    HashWriter writer{TaggedHash(FN_GENESIS_NODE_TAG)};
    writer << std::span<const unsigned char>(left.begin(), left.size());
    writer << std::span<const unsigned char>(right.begin(), right.size());
    return writer.GetSHA256();
}

} // namespace fn_genesis_detail

//! Check the canonical row-count, raw-PoD ordering and uniqueness rules.
inline bool ValidateFnGenesisManifest(
    const std::span<const Consensus::FnGenesisRight> manifest,
    std::string& error)
{
    error.clear();
    if (manifest.empty()) {
        error = "fn-genesis-manifest-empty";
        return false;
    }
    if (manifest.size() > Consensus::MAX_FN_EVER_ISSUED) {
        error = "fn-genesis-manifest-too-large";
        return false;
    }
    for (size_t i{1}; i < manifest.size(); ++i) {
        const int order{manifest[i - 1].pod_id.Compare(manifest[i].pod_id)};
        if (order == 0) {
            error = "fn-genesis-manifest-duplicate-pod";
            return false;
        }
        if (order > 0) {
            error = "fn-genesis-manifest-not-raw-sorted";
            return false;
        }
    }
    return true;
}

//! Hash one manifest row at an explicit index and under an explicit context.
//! This low-level primitive intentionally accepts any version/count so tests
//! and future versioned readers can verify that those fields are committed.
inline uint256 FnGenesisManifestLeaf(
    const uint256& chain_domain,
    const uint32_t fn_genesis_height,
    const uint16_t manifest_version,
    const uint32_t row_count,
    const uint32_t row_index,
    const Consensus::FnGenesisRight& row)
{
    const auto context{fn_genesis_detail::EncodeContext(
        chain_domain, fn_genesis_height, manifest_version, row_count)};
    unsigned char index[4];
    WriteBE32(index, row_index);

    HashWriter writer{TaggedHash(FN_GENESIS_LEAF_TAG)};
    writer << std::span<const unsigned char>(context);
    writer << std::span<const unsigned char>(index, sizeof(index));
    writer << std::span<const unsigned char>(row.pod_id.begin(), row.pod_id.size());
    writer << std::span<const unsigned char>(row.recipient_key_hash);
    return writer.GetSHA256();
}

/**
 * Compute a manifest commitment after enforcing canonicality.
 *
 * A null chain domain fails closed. `manifest_version` is committed as data;
 * callers selecting v1 pass FN_GENESIS_MANIFEST_VERSION_V1. Accepting an
 * explicit value here is intentional: it makes version binding independently
 * testable. It does not define or accept a future consensus version; a v1
 * consensus caller must require the pinned value or use the V1 wrapper below.
 */
inline std::optional<uint256> ComputeFnGenesisManifestRoot(
    const uint256& chain_domain,
    const uint32_t fn_genesis_height,
    const uint16_t manifest_version,
    const std::span<const Consensus::FnGenesisRight> manifest,
    std::string* error = nullptr)
{
    std::string local_error;
    if (chain_domain.IsNull()) {
        local_error = "fn-genesis-manifest-null-chain-domain";
        if (error) *error = local_error;
        return std::nullopt;
    }
    if (!ValidateFnGenesisManifest(manifest, local_error)) {
        if (error) *error = local_error;
        return std::nullopt;
    }
    if (error) error->clear();

    const uint32_t count{static_cast<uint32_t>(manifest.size())};
    std::vector<uint256> level;
    level.reserve(manifest.size());
    for (uint32_t i{0}; i < count; ++i) {
        level.push_back(FnGenesisManifestLeaf(
            chain_domain, fn_genesis_height, manifest_version, count, i, manifest[i]));
    }

    while (level.size() > 1) {
        if (level.size() % 2 != 0) level.push_back(level.back());
        for (size_t i{0}; i < level.size(); i += 2) {
            level[i / 2] = fn_genesis_detail::NodeHash(level[i], level[i + 1]);
        }
        level.resize(level.size() / 2);
    }

    const auto context{fn_genesis_detail::EncodeContext(
        chain_domain, fn_genesis_height, manifest_version, count)};
    HashWriter writer{TaggedHash(FN_GENESIS_ROOT_TAG)};
    writer << std::span<const unsigned char>(context);
    writer << std::span<const unsigned char>(level.front().begin(), level.front().size());
    return writer.GetSHA256();
}

//! Convenience wrapper for the only defined manifest version.
inline std::optional<uint256> ComputeFnGenesisManifestRootV1(
    const uint256& chain_domain,
    const uint32_t fn_genesis_height,
    const std::span<const Consensus::FnGenesisRight> manifest,
    std::string* error = nullptr)
{
    return ComputeFnGenesisManifestRoot(
        chain_domain, fn_genesis_height, FN_GENESIS_MANIFEST_VERSION_V1, manifest, error);
}

/** Encode the canonical v1 release artifact after recomputing its root. */
inline std::optional<std::vector<unsigned char>> EncodeFnGenesisManifestFileV1(
    const uint256& chain_domain,
    const uint32_t fn_genesis_height,
    const uint16_t manifest_version,
    const std::span<const Consensus::FnGenesisRight> manifest,
    const uint256& expected_root,
    std::string* error = nullptr)
{
    const auto fail{[error](const std::string& reason)
        -> std::optional<std::vector<unsigned char>> {
        if (error) *error = reason;
        return std::nullopt;
    }};
    if (manifest_version != FN_GENESIS_MANIFEST_VERSION_V1) {
        return fail("fn-genesis-manifest-file-unsupported-version");
    }

    std::string root_error;
    const auto computed_root{ComputeFnGenesisManifestRootV1(
        chain_domain, fn_genesis_height, manifest, &root_error)};
    if (!computed_root) return fail(root_error);
    if (expected_root.IsNull()) {
        return fail("fn-genesis-manifest-file-null-root");
    }
    if (*computed_root != expected_root) {
        return fail("fn-genesis-manifest-file-root-mismatch");
    }

    const auto context{fn_genesis_detail::EncodeContext(
        chain_domain, fn_genesis_height, manifest_version,
        static_cast<uint32_t>(manifest.size()))};
    constexpr size_t ROW_SIZE{32 + 20};
    std::vector<unsigned char> bytes;
    bytes.reserve(FN_GENESIS_MANIFEST_FILE_MAGIC.size() + context.size() +
                  expected_root.size() + ROW_SIZE * manifest.size());
    bytes.insert(bytes.end(), FN_GENESIS_MANIFEST_FILE_MAGIC.begin(),
                 FN_GENESIS_MANIFEST_FILE_MAGIC.end());
    bytes.insert(bytes.end(), context.begin(), context.end());
    bytes.insert(bytes.end(), expected_root.begin(), expected_root.end());
    for (const Consensus::FnGenesisRight& row : manifest) {
        bytes.insert(bytes.end(), row.pod_id.begin(), row.pod_id.end());
        bytes.insert(bytes.end(), row.recipient_key_hash.begin(),
                     row.recipient_key_hash.end());
    }
    if (error) error->clear();
    return bytes;
}

//! Standard (non-reversed) SHA-256 digest bytes for publication/sha256sum.
inline std::array<unsigned char, CSHA256::OUTPUT_SIZE>
FnGenesisManifestFileSha256(const std::span<const unsigned char> bytes)
{
    std::array<unsigned char, CSHA256::OUTPUT_SIZE> digest{};
    CSHA256().Write(bytes.data(), bytes.size()).Finalize(digest.data());
    return digest;
}

//! Reconstruct the exact 25-byte legacy P2PKH script designated by a row. A
//! genesis output uses ordinary coinbase spend semantics: the standard
//! coinbase maturity is the only delay; FN adds no separate transfer lock.
inline CScript FnGenesisRecipientScript(const Consensus::FnGenesisRight& row)
{
    return CScript() << OP_DUP << OP_HASH160
                     << std::vector<unsigned char>(row.recipient_key_hash.begin(),
                                                   row.recipient_key_hash.end())
                     << OP_EQUALVERIFY << OP_CHECKSIG;
}

} // namespace modern

#endif // B3COIN_MODERN_FN_GENESIS_H
