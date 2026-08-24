// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_STAKING_H
#define B3COIN_NODE_STAKING_H

#include <interfaces/chain.h>
#include <crypto/bls.h>
#include <key.h>
#include <script/script.h>
#include <sync.h>
#include <uint256.h>

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

namespace node {

/**
 * The automatic Modern PoS staking loop (release-v1 validator UX, owner
 * ruling 2026-08-23: "automatic staking loop ... nothing more advanced in
 * V1").
 *
 * One thread, one validator key. For every new tip it asks the block
 * assembler for the deterministic modern-PoS template of this validator
 * (the assembler resolves the smallest eligible recovery round and forces
 * the exact round timestamp, frozen V1 spec §3-§4), waits until wall-clock
 * reaches that timestamp (the network refuses earlier blocks as
 * time-too-new), rebuilds the template with the latest transactions, signs
 * it with the validator key (spec §5) and submits it. A tip change while
 * waiting restarts the computation; any error is recorded and retried.
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
    StakingLoop(ChainstateManager& chainman, CTxMemPool* mempool);
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
                        std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool ClearFinalityKey(std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool HasFinalityKey() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex) { LOCK(m_mutex); return m_bls_key.has_value(); }

    //! Start staking with `validator_key`; block fees pay `coinbase_script`.
    //! Returns false (with `error`) if already running or the key is unusable.
    bool Start(const CKey& validator_key, const CScript& coinbase_script, std::string& error) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Stop and join the loop (idempotent).
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Current status; stake weights are reported for `validator_key` (x-only)
    //! if given, else for the loop's own key.
    interfaces::StakingStatus Status(const std::optional<std::array<unsigned char, 32>>& validator_key) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    void ThreadLoop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Sleep up to `d` unless stopped; returns false when stop was requested.
    bool SleepUnlessStopped(std::chrono::milliseconds d) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void SetState(const std::string& state, const std::string& error = "") EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Fill the chain-fact fields of a status report for `key`.
    void FillChainFacts(interfaces::StakingStatus& status, const std::optional<std::array<unsigned char, 32>>& key);

    ChainstateManager& m_chainman;
    CTxMemPool* const m_mempool;
    PeerManager* m_peerman{nullptr};

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
};

} // namespace node

#endif // B3COIN_NODE_STAKING_H
