// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The B3Coin developers
// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_LEGACY_CONSENSUS_H
#define B3COIN_LEGACY_CONSENSUS_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <script/script.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

class CBlock;
class CBlockIndex;
class CCoinsViewCache;
class CTransaction;

namespace legacy {

/** Consensus constants frozen from the existing B3Coin chain. */
inline constexpr uint32_t MAX_BLOCK_SIZE{5'000'000};
inline constexpr uint32_t MAX_BLOCK_SIGOPS{MAX_BLOCK_SIZE / 100};
inline constexpr uint32_t COINBASE_MATURITY{30};
inline constexpr uint32_t STAKE_MIN_CONFIRMATIONS{10};
inline constexpr uint32_t STAKE_MIN_AGE{60 * 60};
inline constexpr uint32_t STAKE_MODIFIER_INTERVAL{15 * 60};
inline constexpr uint32_t STAKE_MODIFIER_INTERVAL_RATIO{3};
inline constexpr uint32_t STAKE_TARGET_SPACING{360};
inline constexpr uint32_t TARGET_TIMESPAN{STAKE_TARGET_SPACING * 20};
inline constexpr uint32_t TARGET_SPACING_WORK_MAX{STAKE_TARGET_SPACING * 3};
inline constexpr uint32_t MAX_FUTURE_BLOCK_TIME{10 * 60};
inline constexpr uint32_t MAX_FUTURE_COINBASE_TIME_POW{100'000};
/**
 * The final historical B3Coin daemon rejects peers advertising a protocol
 * version below 80006. Advertise its final protocol version during the
 * preserved legacy-chain handshake, while keeping Bitcoin Core's own version
 * unchanged for every non-legacy chain.
 */
inline constexpr int P2P_PROTOCOL_VERSION{80'008};
/**
 * The legacy protocol's version numbering diverged from Bitcoin Core's. Cap
 * Core feature negotiation below SENDHEADERS_VERSION so an 80008 B3Coin peer
 * is not sent post-legacy Core-only messages such as sendaddrv2, wtxidrelay,
 * sendcmpct, sendheaders, or feefilter.
 */
inline constexpr int P2P_COMPATIBILITY_VERSION{70'011};
inline constexpr CAmount CENT{COIN / 100};
inline constexpr CAmount COIN_YEAR_REWARD{CENT};
/**
 * Height-tiered Fundamental Node collateral of the historical client
 * (fn-activity.h GetFNCollateral in the old tree): 25M B3 through height
 * 85000, 20M through 105000, 15M afterwards. The collateral was destroyed
 * by proof of integration: the transaction claims less than it spends by
 * at least this amount, so the value appears in no output at all (unlike
 * proof of burn, which parks coins on an unspendable address). Exactly the
 * collateral is excluded from the fee. Only this accounting is preserved;
 * it enables no Fundamental Node service, payment, vote, or validation
 * mechanism.
 */
CAmount GetFNCollateral(int height);

/**
 * True when a height must use the preserved B3Coin consensus rules.
 * Transitional wrapper over Consensus::GetB3Era() in consensus/era.h,
 * which is the single source of truth; new code should call that
 * directly where the height is unambiguous.
 */
bool IsActive(const Consensus::Params& params, int height);

/**
 * Restore the historical genesis block's stake-modifier state whenever its
 * index entry is created or reconstructed. This is required before block 1
 * can compute its inherited modifier during a reindex.
 */
void InitializeGenesisBlockIndex(CBlockIndex& index, const uint256& proof_hash);

/** Old hybrid PoW/PoS per-block target adjustment. */
uint32_t GetNextTargetRequired(const CBlockIndex* pindex_last, bool proof_of_stake,
                               const Consensus::Params& params);

/** Validate the old post-transaction signature on proof-of-stake blocks. */
bool CheckBlockSignature(const CBlock& block);

struct StakeProof {
    uint256 hash;
    uint64_t coin_day_weight;
};

/**
 * Validate the old Peercoin-v1 stake kernel. Script validation remains in the
 * normal Core validation path so every coinstake input is checked uniformly.
 */
std::optional<StakeProof> CheckStakeKernel(const CBlockIndex* pindex_prev,
                                           const CTransaction& tx,
                                           const CCoinsViewCache& view,
                                           uint32_t bits);

/** Compute coin age in old-chain coin-days for a coinstake transaction. */
std::optional<uint64_t> GetCoinAge(const CTransaction& tx,
                                   const CBlockIndex* pindex_prev,
                                   const CCoinsViewCache& view);

/** Rebuild the Peercoin-v1 stake modifier for a newly-connected block. */
bool ComputeNextStakeModifier(const CBlockIndex* pindex_prev,
                              uint64_t& stake_modifier,
                              bool& generated);

/** Legacy issuance rules, with Fundamental Node logic intentionally omitted. */
CAmount GetProofOfWorkReward(CAmount fees, int height,
                             const Consensus::Params& params);
CAmount GetProofOfStakeReward(const CBlockIndex* pindex_prev,
                              uint64_t coin_age, CAmount fees);

/** Return the fee amount the legacy block-reward calculation counted. */
CAmount GetLegacyTransactionFee(CAmount value_in, CAmount value_out, bool is_coinstake, int height);

/**
 * The historical repair window: the final client skips its entire coinstake
 * reward-cap check for blocks at heights 77447..77505 inclusive
 * (master:src/main.cpp, "if (!(pindex->nHeight > 77446 && pindex->nHeight <
 * 77506))"). The dense hardened checkpoints at 77900-78961 and the
 * restricted-staker rule below are the rest of the same incident response.
 */
bool IsRepairWindowHeight(int height);

/**
 * The historical superblock payment bound (main.h SUPERBLOCKPAYMENT =
 * 75656908 * KILO_COIN, KILO_COIN = 1e9). At the superblock height the last
 * coinstake output must pay at most this to SuperblockPayeeScript().
 */
inline constexpr CAmount LEGACY_SUPERBLOCK_PAYMENT{75'656'908LL * 1'000'000'000LL};

/**
 * The P2PKH script of the superblock public key's hash, exactly as the final
 * client built it (superlockPayee.SetDestination(superblockPubkey.GetID())).
 */
CScript SuperblockPayeeScript(const std::vector<unsigned char>& pubkey);

/**
 * From this height (exclusive) the final client refuses any proof-of-stake
 * block whose second coinstake output pays the restricted destination while
 * earning a positive reward (master:src/main.cpp, "if(pindex->nHeight >
 * 78000)"). The address is hardcoded in the client's ConnectBlock.
 */
inline constexpr int LEGACY_RESTRICTED_STAKE_HEIGHT{78'000};

/**
 * True if the script pays the restricted staking destination under the old
 * client's ExtractDestination semantics: pay-to-pubkey-hash of that key id,
 * or pay-to-pubkey whose key hashes to it (the 0.8-era Solver mapped both to
 * the same CBitcoinAddress). Modern ExtractDestination no longer folds
 * pay-to-pubkey into a key hash, so this must not use it.
 */
bool StakeDestinationMatches(const CScript& script_pub_key, const uint160& key_id);
bool StakeDestinationIsRestricted(const CScript& script_pub_key);

/** Key hash of the restricted staking address ShJsVNBQMa2M7cfCVPzRMt8nVZxHitBp7v. */
const uint160& RestrictedStakeKeyId();

/**
 * Historical hardened checkpoints of the B3 mainnet chain and the rolling
 * reorg-depth span (nCheckpointSpan), ported verbatim from the historical
 * client's checkpoints.cpp. Installed into CMainParams; other chains carry
 * their own (test chains have none). Never invent new heights or a new span.
 */
inline constexpr int LEGACY_CHECKPOINT_SPAN{500};
const std::map<int, uint256>& MainnetCheckpoints();

/**
 * Live-legacy hardened checkpoint rule (Checkpoints::CheckHardened). Returns
 * false only when `height` is a pinned checkpoint and `hash` is not the pinned
 * value; heights with no checkpoint always pass. A false result is a hard,
 * bannable rejection in the historical client (DoS 100).
 */
bool CheckpointAllows(const Consensus::Params& params, int height, const uint256& hash);

/**
 * Live-legacy rolling deep-reorg rule (Checkpoints::CheckSync). Returns true
 * when a block at `block_height` is too deep to accept given the active tip at
 * `active_tip_height`: it lies at or below active_tip_height - span, so
 * accepting it would reorganize more than `span` blocks. The span comes from
 * params (zero disables the rule). A true result rejects the block WITHOUT a
 * peer penalty in the historical client. Must not be consulted during trusted
 * replay of the settled pre-X prefix.
 */
bool ReorgDepthExceeded(const Consensus::Params& params, int block_height, int active_tip_height);

} // namespace legacy

#endif // B3COIN_LEGACY_CONSENSUS_H
