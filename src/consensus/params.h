// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_PARAMS_H
#define BITCOIN_CONSENSUS_PARAMS_H

#include <consensus/modern_pos_params.h>
#include <script/verify_flags.h>
#include <uint256.h>

#include <array>
#include <chrono>
#include <limits>
#include <map>
#include <optional>
#include <vector>

namespace modern { class PosValidator; }

namespace Consensus {

/**
 * A buried deployment is one where the height of the activation has been hardcoded into
 * the client implementation long after the consensus change has activated. See BIP 90.
 */
enum BuriedDeployment : int16_t {
    // buried deployments get negative values to avoid overlap with DeploymentPos
    DEPLOYMENT_HEIGHTINCB = std::numeric_limits<int16_t>::min(),
    DEPLOYMENT_CLTV,
    DEPLOYMENT_DERSIG,
    DEPLOYMENT_CSV,
    DEPLOYMENT_SEGWIT,
};
constexpr bool ValidDeployment(BuriedDeployment dep) { return dep <= DEPLOYMENT_SEGWIT; }

enum DeploymentPos : uint16_t {
    DEPLOYMENT_TESTDUMMY,
    DEPLOYMENT_TAPROOT, // Deployment of Schnorr/Taproot (BIPs 340-342)
    // NOTE: Also add new deployments to VersionBitsDeploymentInfo in deploymentinfo.cpp
    MAX_VERSION_BITS_DEPLOYMENTS
};
constexpr bool ValidDeployment(DeploymentPos dep) { return dep < MAX_VERSION_BITS_DEPLOYMENTS; }

/**
 * Struct for each individual consensus rule change using BIP9.
 */
struct BIP9Deployment {
    /** Bit position to select the particular bit in nVersion. */
    int bit{28};
    /** Start MedianTime for version bits miner confirmation. Can be a date in the past */
    int64_t nStartTime{NEVER_ACTIVE};
    /** Timeout/expiry MedianTime for the deployment attempt. */
    int64_t nTimeout{NEVER_ACTIVE};
    /** If lock in occurs, delay activation until at least this block
     *  height.  Note that activation will only occur on a retarget
     *  boundary.
     */
    int min_activation_height{0};
    /** Period of blocks to check signalling in (usually retarget period, ie params.DifficultyAdjustmentInterval()) */
    uint32_t period{2016};
    /**
     * Minimum blocks including miner confirmation of the total of 2016 blocks in a retargeting period,
     * which is also used for BIP9 deployments.
     * Examples: 1916 for 95%, 1512 for testchains.
     */
    uint32_t threshold{1916};

    /** Constant for nTimeout very far in the future. */
    static constexpr int64_t NO_TIMEOUT = std::numeric_limits<int64_t>::max();

    /** Special value for nStartTime indicating that the deployment is always active.
     *  This is useful for testing, as it means tests don't need to deal with the activation
     *  process (which takes at least 3 BIP9 intervals). Only tests that specifically test the
     *  behaviour during activation cannot use this. */
    static constexpr int64_t ALWAYS_ACTIVE = -1;

    /** Special value for nStartTime indicating that the deployment is never active.
     *  This is useful for integrating the code changes for a new feature
     *  prior to deploying it on some or all networks. */
    static constexpr int64_t NEVER_ACTIVE = -2;
};

/**
 * Parameters that influence chain consensus.
 */
struct Params {
    uint256 hashGenesisBlock;
    int nSubsidyHalvingInterval;
    /**
     * Enables historical B3Coin consensus before the configured hard fork.
     * This is a network property, not a run-time option: nodes must agree on
     * legacy transaction serialization, scrypt PoW, and hybrid PoW/PoS rules.
     */
    bool legacy_b3coin{false};
    /** Last height at which historical B3Coin proof-of-work is allowed. */
    int legacy_last_pow_block{500};
    /**
     * First block height governed by post-fork consensus. When unset, the
     * hard fork is disabled and all heights remain in the legacy era.
     *
     * This must be pinned together with the activation block hash before a
     * production release enables post-fork validation.
     */
    std::optional<int> hard_fork_height;
    /**
     * LEGACY_FINAL_HASH (X): the exact hash required of the final legacy
     * block at LEGACY_FINAL_HEIGHT (hard_fork_height - 1). When set together
     * with hard_fork_height, the block at H must hash to X, the block at
     * H + 1 must reference X, and no disconnect or reorganization may cross
     * H. Unset until a boundary is explicitly finalized.
     */
    std::optional<uint256> legacy_final_hash;
    /**
     * TRANSITION_LENGTH: the number of temporary-PoW corridor blocks between
     * the final legacy block H and the first modern-PoS block. Heights
     * H+1 .. H+transition_pow_length are the corridor: modern-format blocks
     * whose production consensus is the historical B3 scrypt PoW (used only
     * as the eligibility hash against nBits; block identity stays in the
     * modern hash domain). Height H+transition_pow_length+1 is the first
     * modern-PoS block. 0 disables the corridor (modern PoS directly after
     * H). The locked mainnet design length is 1000; it is inert until a
     * mainnet H/X boundary is finalized.
     */
    int transition_pow_length{0};
    /**
     * Corridor PACING (owner ruling 2026-08-23): a corridor block's timestamp
     * must be at least this many seconds after its parent's, and at most
     * `transition_pow_max_future` seconds ahead of the validating node's
     * clock. Together they bound the corridor's wall-clock duration from
     * below (>= length * spacing - max_future) regardless of hashpower, so a
     * large miner cannot compress the window operators need to create and
     * mature STAKE outputs. Consensus rules for every corridor.
     */
    int64_t transition_pow_min_spacing{60};
    int64_t transition_pow_max_future{120};
    /**
     * The compact-bits target every temporary-PoW corridor block must carry
     * (constant corridor difficulty). The block's scrypt eligibility hash
     * must not exceed this target. Unset means the corridor difficulty
     * policy is unresolved and corridor blocks FAIL CLOSED: regtest fixtures
     * set a trivially easy value as test scaffolding; the mainnet corridor
     * difficulty policy is an OPEN design decision and must not be chosen
     * here silently.
     */
    std::optional<uint32_t> transition_pow_bits;
    /**
     * The subsidy a temporary-PoW corridor coinbase may claim on top of
     * fees. RATIFIED (owner ruling 2026-08-21): 0 -- fees only -- on
     * mainnet; corridor miners' incentive is bootstrapping their own
     * validator position, not issuance. Unset FAILS CLOSED (corridor
     * blocks are refused), so the reward can never ship as an accidental
     * default: every network states its value explicitly.
     */
    std::optional<int64_t> transition_pow_reward;
    /**
     * MIN_STAKE_AMOUNT: the smallest principal a v1 STAKE output may carry.
     * Unset means the stake economics are unresolved and stake creation
     * FAILS CLOSED (mainnet: OPEN decision); regtest fixtures set a small
     * scaffolding value.
     */
    std::optional<int64_t> min_stake_amount;
    /**
     * Modern PoS V1 parameter block (consensus/modern_pos_params.h). Unset
     * means modern-PoS rules are unconfigured and every modern-PoS block
     * FAILS CLOSED (`no-modern-pos-rules`). Real chainparams never set it:
     * every value inside is REVISABLE_BEFORE_MAINNET scaffolding until the
     * owner ratifies mainnet numbers (a guard test pins this).
     */
    std::optional<ModernPosParams> modern_pos;
    /**
     * TEST-ONLY override of the historical FN disintegration collateral
     * (Proof of Disintegration). Unset everywhere except regtest fixtures:
     * the mainnet historical collateral schedule (25M/20M/15M B3 by height)
     * is consensus history and must never change.
     */
    std::optional<int64_t> legacy_fn_collateral_test_override;
    /**
     * Historical hardened checkpoints of the live legacy chain (height -> exact
     * block hash), ported verbatim from the historical client. A legacy block
     * accepted at a pinned height must hash to the pinned value. Empty on chains
     * with no legacy history. Applies to live legacy validation only, never to
     * post-X trusted replay (which verifies its own configured checkpoints).
     */
    std::map<int, uint256> legacy_checkpoints;
    /**
     * Rolling maximum reorg depth for the live legacy chain (the historical
     * client's nCheckpointSpan). A legacy block whose height is at least this
     * far below the active tip is refused (without peer penalty). Zero disables
     * the rule. Live legacy validation only; never applied during trusted
     * replay of the settled pre-X prefix or in the modern era.
     */
    int legacy_checkpoint_span{0};
    /**
     * The historical client's one-off superblock (chainparams nSuperBlockHeight
     * and vSuperBlockPubKey). At exactly this height the general coinstake
     * reward cap is replaced by a structured rule: the last coinstake output
     * must pay at most legacy::LEGACY_SUPERBLOCK_PAYMENT to the P2PKH script of
     * this public key. Unset on chains without one. Live legacy validation
     * only; trusted replay does not adjudicate rewards.
     */
    std::optional<int> legacy_superblock_height;
    std::vector<unsigned char> legacy_superblock_pubkey;
    /**
     * Test-only injection points, never set by real chainparams. They replace
     * former process-global mutable statics so a test override lives on the
     * chain instance (a test constructs its own Params), never in the
     * production binary's global state, and cannot be flipped by anything but
     * code holding a mutable Params -- which production never does after init.
     *
     * - test_only_modern_pos_validator: an installed modern proof-of-stake
     *   rule set. Null in production, so modern PoS stays fail-closed
     *   (CheckModernStake rejects every block) until a real rule set ships.
     * - test_only_asset_policies_active: activates the coloured-asset policy
     *   set (BURN / DEX_VAULT and the conservation rules). False in
     *   production, so those policies stay unactivated and therefore invalid.
     * - test_only_metadata_cells_active: activates the v1 metadata-cell
     *   carrier for policy types 6/7/8 (modern/metadata_cell.h) so fixtures
     *   can exercise UTXO exclusion and undo symmetry. False in production:
     *   every claiming output stays invalid until the real per-type
     *   activation rules ship.
     */
    const modern::PosValidator* test_only_modern_pos_validator{nullptr};
    bool test_only_asset_policies_active{false};
    bool test_only_metadata_cells_active{false};
    /**
     * test_only_mpa_active: activates the Modern Payload Area consensus
     * context (modern/mpa.h) and, with it, the FINALITY_KEY_EVIDENCE record
     * type 5, so fixtures can exercise cell<->evidence binding. False in
     * production: any transaction carrying an MPA stays invalid.
     */
    bool test_only_mpa_active{false};
    /**
     * Hashes of blocks that
     * - are known to be consensus valid, and
     * - buried in the chain, and
     * - fail if the default script verify flags are applied.
     */
    std::map<uint256, script_verify_flags> script_flag_exceptions;
    /** Block height and hash at which BIP34 becomes active */
    int BIP34Height;
    uint256 BIP34Hash;
    /** Block height at which BIP65 becomes active */
    int BIP65Height;
    /** Block height at which BIP66 becomes active */
    int BIP66Height;
    /** Block height at which CSV (BIP68, BIP112 and BIP113) becomes active */
    int CSVHeight;
    /** Block height at which Segwit (BIP141, BIP143 and BIP147) becomes active.
     * Note that segwit v0 script rules are enforced on all blocks except the
     * BIP 16 exception blocks. */
    int SegwitHeight;
    /** Don't warn about unknown BIP 9 activations below this height.
     * This prevents us from warning about the CSV and segwit activations. */
    int MinBIP9WarningHeight;
    std::array<BIP9Deployment,MAX_VERSION_BITS_DEPLOYMENTS> vDeployments;
    /** Proof of work parameters */
    uint256 powLimit;
    bool fPowAllowMinDifficultyBlocks;
    /**
      * Enforce BIP94 timewarp attack mitigation. On testnet4 this also enforces
      * the block storm mitigation.
      */
    bool enforce_BIP94;
    bool fPowNoRetargeting;
    int64_t nPowTargetSpacing;
    int64_t nPowTargetTimespan;
    std::chrono::seconds PowTargetSpacing() const
    {
        return std::chrono::seconds{nPowTargetSpacing};
    }
    int64_t DifficultyAdjustmentInterval() const { return nPowTargetTimespan / nPowTargetSpacing; }
    /** The best chain should have at least this much work */
    uint256 nMinimumChainWork;
    /** By default assume that the signatures in ancestors of this block are valid */
    uint256 defaultAssumeValid;

    /**
     * If true, witness commitments contain a payload equal to a Bitcoin Script solution
     * to the signet challenge. See BIP325.
     */
    bool signet_blocks{false};
    std::vector<uint8_t> signet_challenge;

    int DeploymentHeight(BuriedDeployment dep) const
    {
        switch (dep) {
        case DEPLOYMENT_HEIGHTINCB:
            return BIP34Height;
        case DEPLOYMENT_CLTV:
            return BIP65Height;
        case DEPLOYMENT_DERSIG:
            return BIP66Height;
        case DEPLOYMENT_CSV:
            return CSVHeight;
        case DEPLOYMENT_SEGWIT:
            return SegwitHeight;
        } // no default case, so the compiler can warn about missing cases
        return std::numeric_limits<int>::max();
    }
};

} // namespace Consensus

#endif // BITCOIN_CONSENSUS_PARAMS_H
