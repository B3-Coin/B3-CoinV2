// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_POS_V1_H
#define B3COIN_MODERN_POS_V1_H

#include <arith_uint256.h>
#include <consensus/amount.h>
#include <consensus/modern_pos_params.h>
#include <consensus/validation.h>
#include <hash.h>
#include <modern/stake.h>
#include <primitives/block.h>
#include <pubkey.h>
#include <script/script.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace modern {

/**
 * Modern PoS V1 — the frozen deterministic rule set
 * (doc/design/b3-modern-pos-spec.md). Pure functions only: every input is
 * explicit, nothing reads global state, and validation and production share
 * the same code paths.
 *
 *   seed_M      = TaggedHash(SEED_TAG, domain || parent_identity)
 *   digest_h    = TaggedHash(ELIG_TAG, domain || seed_h || height || round || key)
 *   seed_(h+1)  = digest_h
 *   eligible    iff digest_h < MAX256 * f0 * 2^round * w / W
 *   nTime       = parent.nTime + interval + round * round_seconds  (EXACT)
 *   signature   = BIP340(validator_key, TaggedHash(SIG_TAG, domain || block_hash))
 */

using PosValidatorKey = std::array<unsigned char, STAKE_VALIDATOR_KEY_SIZE>;

inline constexpr const char* MODERN_POS_SEED_TAG{"B3/MODERN/POS/SEED/V0"};
inline constexpr const char* MODERN_POS_ELIG_TAG{"B3/MODERN/POS/ELIG/V1"};
inline constexpr const char* MODERN_POS_SIG_TAG{"B3/MODERN/POS/SIG/V1"};

//! Length of a modern-PoS block signature (BIP340).
inline constexpr size_t MODERN_POS_SIG_SIZE{64};

/**
 * The eligibility seed consumed by the block at height h. For a parent that
 * is itself a modern-PoS block this is the parent's own eligibility digest
 * (the chained seed); for the first modern-PoS block the parent is the last
 * pre-PoS block (corridor exit, or H itself with no corridor) and the seed
 * derives from that block's identity under the seed domain tag.
 */
inline uint256 ModernPosGenesisSeed(const uint256& chain_domain, const uint256& parent_identity)
{
    HashWriter writer{TaggedHash(MODERN_POS_SEED_TAG)};
    writer << chain_domain << parent_identity;
    return writer.GetSHA256();
}

inline uint256 ModernPosEligibilityDigest(const uint256& chain_domain, const uint256& seed,
                                          const uint32_t height, const uint32_t round,
                                          const PosValidatorKey& key)
{
    HashWriter writer{TaggedHash(MODERN_POS_ELIG_TAG)};
    writer << chain_domain << seed << height << round;
    writer << std::span<const unsigned char>{key};
    return writer.GetSHA256();
}

//! The exact timestamp a block claiming `round` must carry.
inline int64_t ModernPosBlockTime(const int64_t parent_time, const int64_t round,
                                  const Consensus::ModernPosParams& pos)
{
    return parent_time + pos.block_interval_seconds + round * pos.round_seconds;
}

/**
 * Decode the recovery round from an exact timestamp delta. Returns nullopt
 * for any delta that is not block_interval plus a non-negative exact
 * multiple of round_seconds — such a block is invalid (bad-pos-time). There
 * is deliberately no upper round bound: eligibility saturates instead.
 */
inline std::optional<int64_t> DecodeModernPosRound(const int64_t parent_time, const int64_t block_time,
                                                   const Consensus::ModernPosParams& pos)
{
    const int64_t delta{block_time - parent_time - pos.block_interval_seconds};
    if (delta < 0 || delta % pos.round_seconds != 0) return std::nullopt;
    return delta / pos.round_seconds;
}

/**
 * The frozen eligibility comparison, defined EXACTLY as this integer
 * computation (consensus-exact by definition, no floating point):
 *
 *   num = w * f0_num,  den = W * f0_den            (128-bit, exact)
 *   if num * 2^round >= den            -> eligible (saturated round)
 *   T = (MAX256 / den) * (num << round)            (arith_uint256)
 *   eligible iff digest < T
 *
 * The single MAX256/den truncation loses less than den (< 2^96) of a
 * threshold whose scale is 2^256 * num / den — a relative error below
 * 2^-160, and the rule is the computation itself. w = 0 or W = 0 is never
 * eligible.
 */
inline bool ModernPosEligible(const uint256& digest, const CAmount w, const CAmount W,
                              const int64_t round, const Consensus::ModernPosParams& pos)
{
    if (w <= 0 || W <= 0 || w > W || round < 0) return false;
    using u128 = unsigned __int128;
    const u128 num{static_cast<u128>(w) * pos.f0_num};
    const u128 den{static_cast<u128>(W) * pos.f0_den};
    if (num == 0 || den == 0) return false;

    // Saturation: once num * 2^round can no longer be below den, every
    // online validator is eligible — the recovery guarantee.
    if (round >= 128) return true;
    if (num > (~u128{0} >> round)) return true;
    const u128 shifted{num << round};
    if (shifted >= den) return true;

    const auto to_arith{[](const u128 v) {
        arith_uint256 out{static_cast<uint64_t>(v >> 64)};
        out <<= 64;
        out |= arith_uint256{static_cast<uint64_t>(v)};
        return out;
    }};
    const arith_uint256 max256{~arith_uint256{0}};
    const arith_uint256 threshold{(max256 / to_arith(den)) * to_arith(shifted)};
    return UintToArith256(digest) < threshold;
}

//! The message a modern-PoS block signature commits to.
inline uint256 ModernPosSignatureHash(const uint256& chain_domain, const uint256& block_hash)
{
    HashWriter writer{TaggedHash(MODERN_POS_SIG_TAG)};
    writer << chain_domain << block_hash;
    return writer.GetSHA256();
}

/**
 * Extract the validator declaration from a modern-PoS coinbase scriptSig:
 * the BIP34 height push followed immediately by a direct (minimal) 32-byte
 * push of the validator key. Additional trailing data is permitted (miner
 * extranonce convention). Returns nullopt on any malformed declaration.
 */
inline std::optional<PosValidatorKey> ExtractModernPosValidatorKey(const CScript& script_sig)
{
    CScript::const_iterator it{script_sig.begin()};
    opcodetype opcode;
    std::vector<unsigned char> data;
    if (!script_sig.GetOp(it, opcode, data)) return std::nullopt; // BIP34 height push
    if (!script_sig.GetOp(it, opcode, data)) return std::nullopt;
    if (opcode != static_cast<opcodetype>(STAKE_VALIDATOR_KEY_SIZE) ||
        data.size() != STAKE_VALIDATOR_KEY_SIZE) {
        return std::nullopt;
    }
    PosValidatorKey key;
    std::copy(data.begin(), data.end(), key.begin());
    bool nonzero{false};
    for (const unsigned char b : key) nonzero |= (b != 0);
    if (!nonzero) return std::nullopt;
    return key;
}

//! Verify the trailing BIP340 block signature against the declared key.
inline bool VerifyModernPosSignature(const std::vector<unsigned char>& block_sig,
                                     const PosValidatorKey& key, const uint256& sig_hash)
{
    if (block_sig.size() != MODERN_POS_SIG_SIZE) return false;
    const XOnlyPubKey pubkey{std::span<const unsigned char>{key}};
    return pubkey.VerifySchnorr(sig_hash, block_sig);
}

/**
 * Everything the parent chain contributes to judging a modern-PoS block.
 * The seed is the parent's cached eligibility digest when the parent is a
 * modern-PoS block, else ModernPosGenesisSeed(domain, parent identity) —
 * the caller (validation or production) resolves that, because only it can
 * see the parent's phase and index.
 */
struct ModernPosContext {
    uint256 chain_domain{};
    uint256 seed{};
    int height{0};
    int64_t parent_time{0};
    CAmount validator_weight{0}; // w: aggregated ACTIVE stake of the declared key
    CAmount total_weight{0};     // W: total ACTIVE stake
};

/**
 * The full V1 block-level PoS check (spec sections 3-5), shared by
 * validation; production uses the same primitives to construct what this
 * accepts. `check_signature` is false only on the miner's own pre-signature
 * self-test (fJustCheck); every externally received block is verified.
 * On success `digest_out` carries the block's eligibility digest — the next
 * height's seed — for caching on the block index.
 */
inline bool CheckModernPosBlock(const CBlock& block, const ModernPosContext& ctx,
                                const Consensus::ModernPosParams& pos, const bool check_signature,
                                BlockValidationState& state, uint256& digest_out)
{
    const std::optional<int64_t> round{DecodeModernPosRound(ctx.parent_time, block.GetBlockTime(), pos)};
    if (!round) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-pos-time",
                             "modern-PoS timestamp is not the exact round time");
    }
    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase()) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-pos-coinbase",
                             "modern-PoS block has no coinbase");
    }
    const auto key{ExtractModernPosValidatorKey(block.vtx[0]->vin[0].scriptSig)};
    if (!key) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-pos-key",
                             "modern-PoS coinbase does not declare a validator key");
    }
    const uint256 digest{ModernPosEligibilityDigest(ctx.chain_domain, ctx.seed,
                                                    static_cast<uint32_t>(ctx.height),
                                                    static_cast<uint32_t>(*round), *key)};
    if (!ModernPosEligible(digest, ctx.validator_weight, ctx.total_weight, *round, pos)) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-pos-ineligible",
                             "validator is not eligible at the claimed height and round");
    }
    if (check_signature &&
        !VerifyModernPosSignature(block.vchBlockSig, *key,
                                  ModernPosSignatureHash(ctx.chain_domain, block.GetHash()))) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-pos-signature",
                             "modern-PoS block signature is invalid");
    }
    digest_out = digest;
    return true;
}

} // namespace modern

#endif // B3COIN_MODERN_POS_V1_H
