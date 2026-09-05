// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_STAKING_H
#define B3COIN_NODE_STAKING_H

#include <crypto/bls.h>
#include <interfaces/chain.h>
#include <key.h>
#include <script/script.h>
#include <sync.h>
#include <uint256.h>
#include <util/fs.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

class ChainstateManager;
class CTxMemPool;
class PeerManager;

namespace Consensus {
struct ModernPosParams;
}

namespace node {

class ValidatorSetSnapshot;

/**
 * Local, non-consensus proposer coordination for one existing Modern-PoS
 * recovery round. Every consensus-eligible validator gets the same
 * deterministic rank; rank zero sends at the round time and backups wait a
 * short, bounded interval. This value is scheduling policy only: validation,
 * fork choice and relay never consult it.
 */
enum class PreferredProposerAction {
    SCHEDULE,
    WAIT_NEXT_ROUND,
    UNAVAILABLE,
};

struct PreferredProposerPlan {
    PreferredProposerAction action{PreferredProposerAction::UNAVAILABLE};
    uint32_t rank{0};
    uint32_t eligible_count{0};
    std::chrono::milliseconds delay{0};
    std::chrono::milliseconds window{0};
};

/**
 * Compute this validator's advisory send order among the members that are
 * already consensus-eligible at (height, round). Invalid coordination input
 * is distinguished from a local key that is ineligible or whose unique slot
 * does not fit. WAIT_NEXT_ROUND is never permission to back-fill this one.
 */
PreferredProposerPlan ComputePreferredProposerPlan(
    const uint256& chain_domain, const uint256& seed, int height,
    int64_t round, const ValidatorSetSnapshot& set,
    const std::array<unsigned char, 32>& validator_key,
    const Consensus::ModernPosParams& pos);

/**
 * The automatic Modern PoS staking loop (release-v1 validator UX, owner
 * ruling 2026-08-23: "automatic staking loop ... nothing more advanced in
 * V1").
 *
 * One thread, one validator key. For every new tip it derives the current
 * consensus recovery round from wall-clock time, ranks all validators already
 * eligible in that round, and waits for this validator's unique advisory send
 * window. The assembler rechecks eligibility for that exact round and forces
 * its consensus timestamp (frozen V1 spec §3-§4); the loop then signs with
 * the validator key (spec §5) and submits. A tip change or expired send
 * window discards the local candidate and restarts the computation; any error
 * is recorded and retried.
 *
 * It produces nothing while the next block is not a modern-PoS block
 * (legacy era, corridor, unpinned boundary, unconfigured rules), during
 * initial block download, or while this validator has no ACTIVE stake.
 * Everything consensus-relevant lives in the assembler and validation; this
 * class only schedules.
 */
class StakingLoop
{
public:
    StakingLoop(ChainstateManager& chainman, CTxMemPool* mempool,
                fs::path finality_signer_dir);
    ~StakingLoop();

    //! Wire the peer manager for finality-signature relay (after net setup).
    void SetPeerManager(PeerManager* peerman) { m_peerman = peerman; }
    /**
     * Arm the finality signer (plan Commit 16): with a BLS consensus key
     * loaded, the loop signs every scheduled checkpoint it is eligible for
     * (through node::FinalitySigner: active-snapshot key only, once, at
     * depth) and relays the signatures. Refused while running.
     */
    bool SetFinalityKey(const bls::SecretKey& key, const std::array<unsigned char, 32>& validator_key,
                        std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_lifecycle_mutex, !m_mutex);
    bool ClearFinalityKey(std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_lifecycle_mutex, !m_mutex);
    bool HasFinalityKey() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex) { LOCK(m_mutex); return m_bls_key.has_value(); }

    //! Start staking with `validator_key`; block fees pay `coinbase_script`.
    //! Returns false (with `error`) if already running or the key is unusable.
    bool Start(const CKey& validator_key, const CScript& coinbase_script,
               std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_lifecycle_mutex, !m_mutex);
    /**
     * Start staking and atomically replace the optional finality key.
     *
     * Wallet RPC uses this boundary so a second wallet cannot interleave a
     * separate SetFinalityKey() call with Start() and make the loop combine
     * one wallet's validator key with another wallet's BLS secret.
     */
    bool StartWithFinalityKey(const CKey& validator_key, const CScript& coinbase_script,
                              const std::optional<bls::SecretKey>& finality_key,
                              std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_lifecycle_mutex, !m_mutex);
    //! Stop and join the loop (idempotent).
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_lifecycle_mutex, !m_mutex);
    //! Current status; stake weights are reported for `validator_key` (x-only)
    //! if given, else for the loop's own key.
    interfaces::StakingStatus Status(const std::optional<std::array<unsigned char, 32>>& validator_key) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    bool StartImpl(const CKey& validator_key, const CScript& coinbase_script,
                   const std::optional<bls::SecretKey>* finality_key,
                   std::string& error) EXCLUSIVE_LOCKS_REQUIRED(m_lifecycle_mutex, !m_mutex);
    void ThreadLoop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Sleep up to `d` unless stopped; returns false when stop was requested.
    bool SleepUnlessStopped(std::chrono::milliseconds d) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void SetState(const std::string& state, const std::string& error = "") EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Fill the chain-fact fields of a status report for `key`.
    void FillChainFacts(interfaces::StakingStatus& status, const std::optional<std::array<unsigned char, 32>>& key);

    ChainstateManager& m_chainman;
    CTxMemPool* const m_mempool;
    const fs::path m_finality_signer_dir;
    PeerManager* m_peerman{nullptr};

    // Serialize complete start/stop/key-replacement operations. m_mutex alone
    // protects snapshots, but cannot be held while joining the worker thread.
    Mutex m_lifecycle_mutex;
    Mutex m_mutex;
    std::condition_variable_any m_cv;
    std::thread m_thread;
    bool m_running GUARDED_BY(m_mutex){false};
    bool m_stop GUARDED_BY(m_mutex){false};
    CKey m_key GUARDED_BY(m_mutex);
    std::optional<bls::SecretKey> m_bls_key GUARDED_BY(m_mutex);
    std::array<unsigned char, 32> m_validator GUARDED_BY(m_mutex){};
    CScript m_coinbase_script GUARDED_BY(m_mutex);
    std::string m_state GUARDED_BY(m_mutex){"stopped"};
    std::string m_last_error GUARDED_BY(m_mutex);
    int64_t m_blocks_produced GUARDED_BY(m_mutex){0};
    uint256 m_last_block_hash GUARDED_BY(m_mutex);
    int64_t m_next_block_time GUARDED_BY(m_mutex){0};
    int m_last_signed_height GUARDED_BY(m_mutex){-1};
    bool m_finality_signing_failed GUARDED_BY(m_mutex){false};
};

} // namespace node

#endif // B3COIN_NODE_STAKING_H
