// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_FINALITY_SIGNER_STORE_H
#define B3COIN_NODE_FINALITY_SIGNER_STORE_H

#include <modern/finality_key.h>
#include <uint256.h>
#include <util/fs.h>

#include <optional>
#include <limits>
#include <string>

namespace node {

/**
 * Crash-safe anti-equivocation state for one (chain domain, validator key).
 * No private key material is ever written.
 *
 * `last_signed_*` prevents signing the same or a lower height with a
 * different object. `lock_*` additionally prevents signing a higher
 * checkpoint on a non-descendant fork. Normally both advance together. The
 * lock may move ahead independently only after the node has validated a
 * newer, exact-same-set quorum certificate included on the active chain.
 */
struct FinalitySignerState {
    uint256 chain_domain{};
    modern::ValidatorKeyBytes validator_key{};
    int32_t last_signed_height{-1};
    uint256 last_signed_block_hash{};
    uint256 last_signed_digest{};
    int32_t lock_height{-1};
    uint256 lock_block_hash{};
    uint256 lock_digest{};
    uint64_t lock_epoch{std::numeric_limits<uint64_t>::max()};
    uint256 lock_signing_set_hash{};
    uint256 lock_successor_set_hash{};

    friend bool operator==(const FinalitySignerState& a,
                           const FinalitySignerState& b)
    {
        return a.chain_domain == b.chain_domain &&
               a.validator_key == b.validator_key &&
               a.last_signed_height == b.last_signed_height &&
               a.last_signed_block_hash == b.last_signed_block_hash &&
               a.last_signed_digest == b.last_signed_digest &&
               a.lock_height == b.lock_height &&
               a.lock_block_hash == b.lock_block_hash &&
               a.lock_digest == b.lock_digest &&
               a.lock_epoch == b.lock_epoch &&
               a.lock_signing_set_hash == b.lock_signing_set_hash &&
               a.lock_successor_set_hash == b.lock_successor_set_hash;
    }
};

/**
 * Atomic, checksummed journal. Open() distinguishes an absent file from an
 * invalid one; an invalid/mismatched file always fails closed. Every update
 * verifies the on-disk predecessor before fsync + rename-over + directory
 * sync, so a stale process cannot silently roll the watermark backward.
 */
class FinalitySignerStore
{
public:
    bool Open(const fs::path& directory, const uint256& chain_domain,
              const modern::ValidatorKeyBytes& validator_key,
              std::string& error);

    bool IsOpen() const { return m_open; }
    bool IsAbsent() const { return m_open && !m_state.has_value(); }
    const std::optional<FinalitySignerState>& State() const { return m_state; }
    const fs::path& Path() const { return m_path; }
    const uint256& ChainDomain() const { return m_chain_domain; }

    /** Establish a durable pre-signing marker. Only an absent file may become
     * empty; an existing valid state is left untouched. */
    bool InitializeEmpty(std::string& error);

    /** Persist a vote before it is submitted locally or relayed. Equal
     * height is idempotent only for the exact same block and digest. */
    bool CommitSignedCheckpoint(int height, const uint256& block_hash,
                                const uint256& digest, uint64_t epoch,
                                const uint256& signing_set_hash,
                                const uint256& successor_set_hash,
                                std::string& error);

    /** Move only the ancestry lock to a strictly newer, already validated and
     * included finality checkpoint signed by the exact same epoch/set as the
     * old lock. The last actual vote is retained. */
    bool CommitCertifiedAnchor(int height, const uint256& block_hash,
                               const uint256& digest, uint64_t epoch,
                               const uint256& signing_set_hash,
                               const uint256& successor_set_hash,
                               std::string& error);

    static fs::path StatePath(
        const fs::path& directory, const uint256& chain_domain,
        const modern::ValidatorKeyBytes& validator_key);

private:
    bool Commit(const FinalitySignerState& next, std::string& error);

    bool m_open{false};
    fs::path m_path;
    uint256 m_chain_domain{};
    modern::ValidatorKeyBytes m_validator_key{};
    std::optional<FinalitySignerState> m_state;
};

} // namespace node

#endif // B3COIN_NODE_FINALITY_SIGNER_STORE_H
