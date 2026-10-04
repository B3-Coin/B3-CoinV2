// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_MODERN_FINALITY_V2_H
#define B3COIN_MODERN_FINALITY_V2_H

#include <crypto/bls.h>
#include <crypto/common.h>
#include <hash.h>
#include <modern/finality_certificate.h>
#include <modern/finality_schedule.h>
#include <modern/finality_types.h>
#include <modern/withdrawal_tree.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace modern {

/**
 * Fresh finality lineage, digest V2: the CODEC/TYPE and pure-verification
 * layer (b3-bridge-v2-finality-dependency.md sections 6-8; the bytes are owned
 * by b3-bridge-v2-contracts-spec.md section 2, which this header follows
 * exactly and does not restate as a second source of truth):
 *
 *   FinalizedBlockV2 = lineageId(32) || u64 height || blockHash(32)
 *                      || withdrawalRoot(32) || validatorSetHash(32)
 *                      || u64 epoch || u64 checkpointIndex
 *                      || checkpointHistoryRoot(32) || u8 bridgeFlags   (185 bytes)
 *   lineageId        = TaggedHash("B3/FINALITY/LINEAGE/V2",
 *                         chainDomain || u64 GENESIS_HEIGHT || blockHash(R_f-1) || setHash(Set'_0))
 *   digestV2         = TaggedHash("B3/FINALITY/V2", chainDomain || FinalizedBlockV2)
 *   certId           = keccak256(FinalizedBlockV2)
 *   historyLeaf(h,x) = keccak256(u64 h || x)                         (40-byte preimage)
 *   history node     = keccak256(left || right), depth 32, zero-subtree padding
 *                      (the withdrawal tree algorithm, modern/withdrawal_tree.h)
 *   SetHeader        = V1's 110-byte layout with ruleset_version = 2
 *   payload          = FinalizedBlockV2(185) || bitmap(ceil(n/8)) || aggSig(96) (<= 1,305)
 *
 * Integers are big-endian fixed width. Hashes (lineage id, block hash, roots,
 * set hashes) keep their raw uint256 byte order, exactly as V1's
 * FinalizedBlock::Encode does, so the 32 bytes at an offset equal the
 * Solidity bytes32 value. A V1 signature cannot verify under V2: the tag, the
 * signed length and the header ruleset all differ.
 *
 * Nothing here is height-gated. The pure carrier rule at the end of this
 * header (placement, judgement, the type-11 matcher) is consumed by
 * node::FinalityTracker through node/finality_fresh_lineage.h, which derives
 * the fresh epoch state; the tracker, carrier and signer are live only behind
 * the regtest-only -b3freshlineageheight knob (mainnet and testnet stay unset).
 */

// ---------------------------------------------------------------- constants

//! Quorum ruleset identifier carried in every fresh-lineage set header.
inline constexpr uint16_t FINALITY_RULESET_V2{2};
//! Tagged-hash domains (BIP340 TaggedHash convention, SHA-256).
inline constexpr const char* FINALITY_DIGEST_V2_TAG{"B3/FINALITY/V2"};
inline constexpr const char* FINALITY_LINEAGE_V2_TAG{"B3/FINALITY/LINEAGE/V2"};
//! checkpointIndex must stay below 2^32 (exclusive bound, contracts spec section 2.3).
inline constexpr uint64_t MAX_FINALITY_CHECKPOINT_INDEX{uint64_t{1} << 32};
//! bridgeFlags bit 0: B3 closed burns for good; the verifier closes deposits.
inline constexpr uint8_t FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED{0x01};
//! Every bit a decoder accepts; all other bits must be zero.
inline constexpr uint8_t FINALITY_BRIDGE_FLAGS_KNOWN{FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED};
//! Checkpoint-history tree: the withdrawal tree's depth and algorithm.
inline constexpr unsigned FINALITY_HISTORY_DEPTH{WITHDRAWAL_TREE_DEPTH};
static_assert(FINALITY_HISTORY_DEPTH == 32);
inline constexpr uint64_t MAX_FINALITY_HISTORY_LEAVES{MAX_WITHDRAWAL_LEAVES};
inline constexpr size_t FINALITY_HISTORY_LEAF_PREIMAGE_SIZE{8 + 32};
inline constexpr size_t FINALITY_LINEAGE_PREIMAGE_SIZE{32 + 8 + 32 + 32};

using FinalityHistoryPathV2 = std::array<uint256, FINALITY_HISTORY_DEPTH>;

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

// ------------------------------------------------------------------ digests

//! lineageId = TaggedHash("B3/FINALITY/LINEAGE/V2", domain || u64 G || hash(G) || setHash(Set'_0)).
inline uint256 FinalityLineageIdV2(const uint256& chain_domain, const uint64_t genesis_height,
                                   const uint256& genesis_block_hash, const uint256& genesis_set_hash)
{
    std::array<unsigned char, FINALITY_LINEAGE_PREIMAGE_SIZE> pre{};
    unsigned char* p{pre.data()};
    p = std::copy(chain_domain.begin(), chain_domain.end(), p);
    WriteBE64(p, genesis_height); p += 8;
    p = std::copy(genesis_block_hash.begin(), genesis_block_hash.end(), p);
    std::copy(genesis_set_hash.begin(), genesis_set_hash.end(), p);
    HashWriter w{TaggedHash(FINALITY_LINEAGE_V2_TAG)};
    w << std::span<const unsigned char>(pre);
    return w.GetSHA256();
}

//! The message every fresh-lineage member BLS-signs: TaggedHash("B3/FINALITY/V2", domain || FinalizedBlockV2).
inline uint256 FinalityDigestV2(const uint256& chain_domain, const FinalizedBlockV2& fb)
{
    HashWriter w{TaggedHash(FINALITY_DIGEST_V2_TAG)};
    w << std::span<const unsigned char>(chain_domain.begin(), 32);
    w << std::span<const unsigned char>(fb.Encode());
    return w.GetSHA256();
}

//! certId = keccak256(FinalizedBlockV2): equal for every aggregation of one statement.
inline uint256 FinalityCertIdV2(const FinalizedBlockV2& fb)
{
    return Keccak(fb.Encode());
}

// ---------------------------------------------------------- history tree

//! historyLeaf(h, hash) = keccak256(u64 h || hash), keyed by height.
inline uint256 FinalityHistoryLeaf(const uint64_t height, const uint256& block_hash)
{
    std::array<unsigned char, FINALITY_HISTORY_LEAF_PREIMAGE_SIZE> pre{};
    WriteBE64(pre.data(), height);
    std::copy(block_hash.begin(), block_hash.end(), pre.data() + 8);
    return Keccak(pre);
}

//! Checkpoint index of a fresh-lineage grid height: (h - G) / interval when
//! h >= G, (h - G) mod interval == 0 and the index is below 2^32 (G itself is
//! leaf 0; a certificate additionally needs index >= 1). nullopt otherwise.
inline std::optional<uint64_t> FinalityCheckpointIndexV2(const int64_t height, const int64_t genesis_height,
                                                         const int64_t interval)
{
    if (interval <= 0 || height < genesis_height) return std::nullopt;
    const int64_t delta{height - genesis_height};
    if (delta % interval != 0) return std::nullopt;
    const auto index{static_cast<uint64_t>(delta / interval)};
    if (index >= MAX_FINALITY_CHECKPOINT_INDEX) return std::nullopt;
    return index;
}

//! The incremental history accumulator is the withdrawal tree state.
using FinalityHistoryState = WithdrawalTreeState;

inline bool AppendFinalityHistoryLeaf(FinalityHistoryState& state, const uint256& leaf)
{
    return AppendWithdrawalLeaf(state, leaf);
}

//! Root over the given leaves (index order); the empty root is the depth-32
//! zero subtree. nullopt for a null leaf or more than 2^32 leaves.
inline std::optional<uint256> FinalityHistoryRoot(std::span<const uint256> leaves)
{
    if (leaves.size() > MAX_FINALITY_HISTORY_LEAVES) return std::nullopt;
    FinalityHistoryState state;
    for (const uint256& leaf : leaves) {
        if (!AppendFinalityHistoryLeaf(state, leaf)) return std::nullopt;
    }
    return state.root;
}

/**
 * The 32-sibling path of leaf `index` in the tree over `leaves` (the same
 * level loop as bridge::BuildWithdrawalRelayArtifacts). Sets `root` to the
 * tree root on success. nullopt when index >= leaves.size(), a leaf is null
 * or there are more than 2^32 leaves.
 */
inline std::optional<FinalityHistoryPathV2> FinalityHistoryPath(std::span<const uint256> leaves, const uint64_t index,
                                                                uint256& root)
{
    if (leaves.size() > MAX_FINALITY_HISTORY_LEAVES || index >= leaves.size()) return std::nullopt;
    if (std::any_of(leaves.begin(), leaves.end(), [](const uint256& l) { return l.IsNull(); })) return std::nullopt;
    std::vector<uint256> level(leaves.begin(), leaves.end());
    const auto& zeros{WithdrawalZeroHashes()};
    FinalityHistoryPathV2 path{};
    uint64_t cursor{index};
    for (unsigned depth{0}; depth < FINALITY_HISTORY_DEPTH; ++depth) {
        const uint64_t sibling{cursor ^ 1U};
        path[depth] = sibling < level.size() ? level[sibling] : zeros[depth];
        std::vector<uint256> next;
        next.reserve((level.size() + 1) / 2);
        for (size_t i{0}; i < level.size(); i += 2) {
            const uint256& right{i + 1 < level.size() ? level[i + 1] : zeros[depth]};
            next.push_back(WithdrawalNodeHash(level[i], right));
        }
        level = std::move(next);
        cursor >>= 1;
    }
    if (level.size() != 1) return std::nullopt;
    root = level.front();
    return path;
}

//! Fold a leaf up a 32-sibling path: index bit d = 0 hashes (node || sibling),
//! bit d = 1 hashes (sibling || node), as contracts/src/B3StakerBridge.sol
//! release() does. Bits of `index` above 31 are ignored exactly like that
//! loop; callers enforce index < 2^32.
inline uint256 FoldFinalityHistoryPath(const uint256& leaf, const uint64_t index, const FinalityHistoryPathV2& path)
{
    uint256 node{leaf};
    for (unsigned depth{0}; depth < FINALITY_HISTORY_DEPTH; ++depth) {
        node = ((index >> depth) & 1U) == 0 ? WithdrawalNodeHash(node, path[depth])
                                            : WithdrawalNodeHash(path[depth], node);
    }
    return node;
}

// ------------------------------------------------------------- set header

//! The fresh-lineage header of a member set: V1's 110-byte layout with the
//! same n, W, quorum, aggregate key and members_root, ruleset 2 and the fresh
//! epoch. setHash = ValidatorSetHash(RulesetV2Header(...)).
inline ValidatorSetHeader RulesetV2Header(const ValidatorSetHeader& header, const uint64_t epoch)
{
    ValidatorSetHeader out{header};
    out.epoch = epoch;
    out.ruleset_version = FINALITY_RULESET_V2;
    return out;
}

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

// -------------------------------------------------------------- verification

/**
 * Verify one fresh-lineage certificate against its signing set: a
 * line-for-line mirror of VerifyFinalityCertificate (set-view consistency,
 * bitmap shape, successor hash, weighted quorum, headcount
 * floor(2n/3)+1, then FastAggregateVerify) over FinalityDigestV2. The
 * caller passes the ruleset-2 successor hash; lineage id, index, history
 * root, withdrawal root and flags are placement rules, not checked here.
 */
inline CertificateCheck VerifyFinalityCertificateV2(const uint256& chain_domain, const FinalizedBlockV2& fb,
                                                    const FinalityCertificate& cert, const ValidatorSetView& set,
                                                    const uint256& expected_successor_set_hash)
{
    if (set.keys.size() != set.validator_count || set.weights.size() != set.validator_count || set.validator_count == 0) {
        return CertificateCheck::SET_VIEW_INCONSISTENT;
    }
    if (!IsWellFormedSignerBitmap(cert.signer_bitmap, set.validator_count)) return CertificateCheck::MALFORMED_BITMAP;
    if (fb.validator_set_hash != expected_successor_set_hash) return CertificateCheck::WRONG_SUCCESSOR_SET;
    std::vector<bls::VerifiedPublicKey> signers;
    uint64_t signed_weight{0};
    for (uint32_t i = 0; i < set.validator_count; ++i) {
        if (!SignerBit(cert.signer_bitmap, i)) continue;
        signers.push_back(set.keys[i]);
        signed_weight += set.weights[i];
    }
    if (signers.empty()) return CertificateCheck::NO_SIGNERS;
    if (signed_weight < set.quorum_weight) return CertificateCheck::INSUFFICIENT_WEIGHT;
    if (signers.size() < FinalityHeadcountQuorum(set.validator_count)) {
        return CertificateCheck::INSUFFICIENT_HEADCOUNT;
    }
    const auto sig{bls::Signature::Decode(cert.aggregate_sig)};
    if (!sig) return CertificateCheck::BAD_SIGNATURE;
    const uint256 digest{FinalityDigestV2(chain_domain, fb)};
    if (!bls::FastAggregateVerify(signers, std::span<const unsigned char>(digest.begin(), 32), *sig)) {
        return CertificateCheck::BAD_SIGNATURE;
    }
    return CertificateCheck::OK;
}

// ------------------------------------------------- placement and judgement

/**
 * The verifier's view of the fresh-lineage epoch state derived from the chain
 * BELOW the including block, after the fresh rotation and break rules were
 * applied for the including height (node::ProjectFreshLineage). Sets are the
 * committees' verifier views; hashes are ruleset-2 set hashes.
 */
struct FreshLineageEpochView {
    uint256 lineage_id{};
    //! GENESIS_HEIGHT = R_f - 1: history leaf 0 and the grid anchor.
    int genesis_height{-1};
    uint64_t current_epoch{0};
    //! epoch_starts[e] for every e <= current_epoch; [0] == R_f.
    std::vector<int> epoch_starts;
    bool lineage_broken{false};
    //! Highest fresh-certified height on this chain, if any.
    std::optional<uint64_t> finalized_height;
    //! Set'_current and the ruleset-2 hash of Set'_{current+1}.
    const ValidatorSetView* current_set{nullptr};
    uint256 current_set_hash{};
    uint256 next_set_hash{};
    //! Set'_{current-1}, present once current_epoch >= 1.
    const ValidatorSetView* previous_set{nullptr};
};

enum class CertificatePlacementV2 {
    OK,
    NO_FINALITY_SET,             //!< no Set'_0 (fresh lineage not bootstrapped): nothing can be certified
    LINEAGE_BROKEN,              //!< fresh epoch extended past E + its own max extension
    LINEAGE_MISMATCH,            //!< FinalizedBlockV2.lineage_id is not this chain's fresh lineage
    NOT_CHECKPOINT,              //!< height not on the grid strictly above GENESIS_HEIGHT
    INSUFFICIENT_DEPTH,          //!< including height - checkpoint height < CHECKPOINT_DEPTH
    EPOCH_WINDOW,                //!< epoch not in {current, current-1}
    EPOCH_RELATION,              //!< fresh epoch(checkpoint height) != FinalizedBlockV2.epoch
    WRONG_BLOCK_HASH,            //!< checkpoint is not the block at that height on this chain
    CHECKPOINT_INDEX_MISMATCH,   //!< checkpoint_index != (height - GENESIS_HEIGHT) / interval
    FINALITY_REGRESSION,         //!< height not strictly above the highest fresh-certified height
    HISTORY_ROOT_UNAVAILABLE,    //!< the history root over leaves 0..index-1 cannot be derived
    HISTORY_ROOT_MISMATCH,       //!< certificate does not carry the derived history root
    WITHDRAWAL_ROOT_UNAVAILABLE, //!< the expected withdrawal root cannot be derived
    WITHDRAWAL_ROOT_MISMATCH,    //!< certificate does not carry the expected withdrawal root
    BRIDGE_FLAGS_UNAVAILABLE,    //!< the expected bridge flags cannot be derived
    BRIDGE_FLAGS_MISMATCH,       //!< certificate does not carry the expected bridge flags
};

inline const char* CertificatePlacementV2Name(const CertificatePlacementV2 p)
{
    switch (p) {
    case CertificatePlacementV2::OK: return "fresh-ok";
    case CertificatePlacementV2::NO_FINALITY_SET: return "fresh-no-finality-set";
    case CertificatePlacementV2::LINEAGE_BROKEN: return "fresh-lineage-broken";
    case CertificatePlacementV2::LINEAGE_MISMATCH: return "fresh-lineage-mismatch";
    case CertificatePlacementV2::NOT_CHECKPOINT: return "fresh-not-checkpoint";
    case CertificatePlacementV2::INSUFFICIENT_DEPTH: return "fresh-insufficient-depth";
    case CertificatePlacementV2::EPOCH_WINDOW: return "fresh-epoch-window";
    case CertificatePlacementV2::EPOCH_RELATION: return "fresh-epoch-relation";
    case CertificatePlacementV2::WRONG_BLOCK_HASH: return "fresh-wrong-block-hash";
    case CertificatePlacementV2::CHECKPOINT_INDEX_MISMATCH: return "fresh-checkpoint-index-mismatch";
    case CertificatePlacementV2::HISTORY_ROOT_UNAVAILABLE: return "fresh-history-root-unavailable";
    case CertificatePlacementV2::HISTORY_ROOT_MISMATCH: return "fresh-history-root-mismatch";
    case CertificatePlacementV2::WITHDRAWAL_ROOT_UNAVAILABLE: return "fresh-withdrawal-root-unavailable";
    case CertificatePlacementV2::WITHDRAWAL_ROOT_MISMATCH: return "fresh-withdrawal-root-mismatch";
    case CertificatePlacementV2::BRIDGE_FLAGS_UNAVAILABLE: return "fresh-bridge-flags-unavailable";
    case CertificatePlacementV2::BRIDGE_FLAGS_MISMATCH: return "fresh-bridge-flags-mismatch";
    case CertificatePlacementV2::FINALITY_REGRESSION: return "fresh-finality-regression";
    }
    return "fresh-unknown";
}

using FreshHashAtFn = std::function<std::optional<uint256>(int)>;
using FreshHistoryRootAtFn = std::function<std::optional<uint256>(uint64_t)>;
using FreshBridgeFlagsAtFn = std::function<std::optional<uint8_t>(int)>;

/**
 * The cheap deterministic part of the fresh-lineage carrier rule, before any
 * cryptography. It mirrors CheckCertificatePlacement and adds the lineage id,
 * the checkpoint index, the history root and the bridge flags. Order:
 *   1 set exists, 2 not broken, 3 lineage id, 4 h <= including height,
 *   5 h > GENESIS_HEIGHT on the grid, 6 depth, 7 window {e, e-1},
 *   8 fresh epoch(h) == epoch, 9 block hash, 10 index == (h - G) / interval,
 *   11 h above the fresh finalized height, 12 history root over leaves
 *   0..index-1, 13 withdrawal root, 14 flags.
 * Unlike V1 the regression check is not last: it needs only the finalized
 * height, while deriving the root of a regressed index (below the
 * accumulator) would rebuild the history tree from leaf 0.
 * `hash_at(h)` is the hash at h on the chain the including block extends;
 * `history_root_at(k)` the derived root over leaves 0..k-1;
 * `withdrawal_root_at(h)` / `bridge_flags_at(h)` the values B3 must sign
 * (zero / 0 until the bridge V2 regime supplies them).
 */
inline CertificatePlacementV2 CheckCertificatePlacementV2(const FinalizedBlockV2& fb, const int including_height,
                                                          const FreshLineageEpochView& view,
                                                          const Consensus::ModernPosParams& pos,
                                                          const FreshHashAtFn& hash_at,
                                                          const FreshHistoryRootAtFn& history_root_at,
                                                          const FreshHashAtFn& withdrawal_root_at,
                                                          const FreshBridgeFlagsAtFn& bridge_flags_at)
{
    if (view.epoch_starts.empty() || view.current_set == nullptr || view.current_set->validator_count == 0) {
        return CertificatePlacementV2::NO_FINALITY_SET;
    }
    if (view.lineage_broken) return CertificatePlacementV2::LINEAGE_BROKEN;
    if (fb.lineage_id != view.lineage_id) return CertificatePlacementV2::LINEAGE_MISMATCH;
    if (including_height < 0 || fb.height > static_cast<uint64_t>(including_height)) {
        return CertificatePlacementV2::INSUFFICIENT_DEPTH;
    }
    const int hc{static_cast<int>(fb.height)};
    const auto index{FinalityCheckpointIndexV2(hc, view.genesis_height, pos.checkpoint_interval)};
    // B3 never signs index 0 (GENESIS_HEIGHT itself is history leaf 0).
    if (view.genesis_height < 0 || !index || *index == 0) return CertificatePlacementV2::NOT_CHECKPOINT;
    if (!CheckpointDepthSatisfied(hc, including_height, pos.checkpoint_depth)) {
        return CertificatePlacementV2::INSUFFICIENT_DEPTH;
    }
    if (!(fb.epoch == view.current_epoch || (view.current_epoch >= 1 && fb.epoch == view.current_epoch - 1))) {
        return CertificatePlacementV2::EPOCH_WINDOW;
    }
    if (view.current_epoch >= 1 && fb.epoch == view.current_epoch - 1 && !view.previous_set) {
        return CertificatePlacementV2::EPOCH_WINDOW;
    }
    const auto epoch_of_hc{EpochOfHeight(view.epoch_starts, hc)};
    if (!epoch_of_hc || *epoch_of_hc != fb.epoch) return CertificatePlacementV2::EPOCH_RELATION;
    const auto expected_hash{hash_at(hc)};
    if (!expected_hash || *expected_hash != fb.block_hash) return CertificatePlacementV2::WRONG_BLOCK_HASH;
    if (fb.checkpoint_index != *index) return CertificatePlacementV2::CHECKPOINT_INDEX_MISMATCH;
    if (view.finalized_height && fb.height <= *view.finalized_height) {
        return CertificatePlacementV2::FINALITY_REGRESSION;
    }
    const auto expected_history_root{history_root_at ? history_root_at(*index) : std::nullopt};
    if (!expected_history_root) return CertificatePlacementV2::HISTORY_ROOT_UNAVAILABLE;
    if (fb.checkpoint_history_root != *expected_history_root) return CertificatePlacementV2::HISTORY_ROOT_MISMATCH;
    const std::optional<uint256> expected_withdrawal_root{
        withdrawal_root_at ? withdrawal_root_at(hc) : std::optional<uint256>{uint256{}}};
    if (!expected_withdrawal_root) return CertificatePlacementV2::WITHDRAWAL_ROOT_UNAVAILABLE;
    if (fb.withdrawal_root != *expected_withdrawal_root) return CertificatePlacementV2::WITHDRAWAL_ROOT_MISMATCH;
    const std::optional<uint8_t> expected_flags{
        bridge_flags_at ? bridge_flags_at(hc) : std::optional<uint8_t>{uint8_t{0}}};
    if (!expected_flags) return CertificatePlacementV2::BRIDGE_FLAGS_UNAVAILABLE;
    if (fb.bridge_flags != *expected_flags) return CertificatePlacementV2::BRIDGE_FLAGS_MISMATCH;
    return CertificatePlacementV2::OK;
}

/**
 * Full judgement of a fresh-lineage certificate included at
 * `including_height`: placement first, then VerifyFinalityCertificateV2
 * against Set'_{fb.epoch} (the current committee, or the previous one for a
 * delayed certificate) with the ruleset-2 hash of Set'_{fb.epoch+1} as the
 * successor, exactly as JudgeFinalityCertificate does for V1. Errors are
 * stable "fresh-..." strings.
 */
inline bool JudgeFinalityCertificateV2(const uint256& chain_domain, const FinalizedBlockV2& fb,
                                       const FinalityCertificate& cert, const int including_height,
                                       const FreshLineageEpochView& view, const Consensus::ModernPosParams& pos,
                                       const FreshHashAtFn& hash_at, const FreshHistoryRootAtFn& history_root_at,
                                       const FreshHashAtFn& withdrawal_root_at,
                                       const FreshBridgeFlagsAtFn& bridge_flags_at, std::string& error)
{
    const auto placement{CheckCertificatePlacementV2(fb, including_height, view, pos, hash_at, history_root_at,
                                                     withdrawal_root_at, bridge_flags_at)};
    if (placement != CertificatePlacementV2::OK) {
        error = CertificatePlacementV2Name(placement);
        return false;
    }
    const bool delayed{fb.epoch != view.current_epoch};
    const ValidatorSetView& signing_set{delayed ? *view.previous_set : *view.current_set};
    const uint256& successor{delayed ? view.current_set_hash : view.next_set_hash};
    const auto check{VerifyFinalityCertificateV2(chain_domain, fb, cert, signing_set, successor)};
    if (check != CertificateCheck::OK) {
        error = std::string{"fresh-"} + CertificateCheckName(check);
        return false;
    }
    return true;
}

//! One matched fresh-lineage certificate record (coinbase MPA type 11).
struct FinalityCertificateV2Record {
    size_t record_index{0};
    FinalizedBlockV2 finalized_block{};
    FinalityCertificate certificate{};
    std::vector<unsigned char> payload{};
};

/**
 * Structural matcher for the fresh-lineage carrier: at most one type-11
 * record in the coinbase, record version 1. There is no metadata cell: the
 * coinbase's MODERN_PAYLOAD_ROOT commitment binds the record. The
 * FinalizedBlockV2 prefix is decoded first, `set_size_for_epoch(epoch)`
 * resolves the signing committee's size n (nullopt => that epoch has no
 * fresh committee on this chain), then the payload is decoded for exactly n.
 * Returns true with `out` empty when the coinbase carries no type-11 record.
 */
inline bool MatchFinalityCertificateV2ForEpoch(const CTransaction& coinbase,
                                               const std::function<std::optional<uint32_t>(uint64_t)>& set_size_for_epoch,
                                               std::optional<FinalityCertificateV2Record>& out, std::string& error)
{
    out.reset();
    std::optional<size_t> found;
    for (size_t i = 0; i < coinbase.mpa.size(); ++i) {
        if (coinbase.mpa[i].payload_type != MPA_TYPE_FINALITY_CERTIFICATE_V2) continue;
        if (found) { error = "fresh-cert-duplicate-record"; return false; }
        found = i;
    }
    if (!found) return true;
    const CMpaRecord& rec{coinbase.mpa[*found]};
    if (rec.payload_version != MPA_VERSION_V1) { error = "fresh-cert-record-version"; return false; }
    if (rec.payload.size() < FinalizedBlockV2::SIZE) { error = "fresh-cert-malformed-payload"; return false; }
    const auto fb{FinalizedBlockV2::Decode(std::span<const unsigned char>(rec.payload).first(FinalizedBlockV2::SIZE))};
    if (!fb) { error = "fresh-cert-malformed-payload"; return false; }
    const auto n{set_size_for_epoch(fb->epoch)};
    if (!n) { error = "fresh-cert-unknown-epoch-set"; return false; }
    auto decoded{DecodeCertificatePayloadV2(rec.payload, *n)};
    if (!decoded) { error = "fresh-cert-malformed-payload"; return false; }
    FinalityCertificateV2Record record;
    record.record_index = *found;
    record.finalized_block = decoded->first;
    record.certificate = std::move(decoded->second);
    record.payload = rec.payload;
    out = std::move(record);
    return true;
}

} // namespace modern

#endif // B3COIN_MODERN_FINALITY_V2_H
