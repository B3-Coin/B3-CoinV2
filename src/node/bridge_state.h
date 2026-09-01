// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_BRIDGE_STATE_H
#define B3COIN_NODE_BRIDGE_STATE_H

#include <bridge/admission.h>
#include <bridge/eth_light_client.h>
#include <consensus/params.h>
#include <primitives/block.h>
#include <primitives/transaction_identifier.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

class CBlockIndex;
class CChain;

namespace node {

class BlockManager;
class BridgeStateIndex;

/** Recent active-chain blocks whose bridge effects can be undone in memory. */
inline constexpr size_t BRIDGE_STATE_UNDO_BLOCKS{288};

/** A receipts-root anchor reached from the verified finalized Ethereum head. */
struct BridgeExecutionAnchor {
    uint64_t block_number{0};
    uint256 block_hash{};
    uint256 receipts_root{};
    uint64_t source_finalized_beacon_slot{0};
    uint64_t execution_timestamp{0};
    int connected_height{-1};
    uint256 connected_block{};

    friend bool operator==(const BridgeExecutionAnchor&,
                           const BridgeExecutionAnchor&) = default;
};

/** A type-10 MINT authorization bound to the full evidence-bearing tx bytes. */
struct BridgeTxMintAuthorization {
    Ptxid transaction_id{};
    uint32_t output_index{0};
    bridge::BridgeMintAuthorization authorization{};

    friend bool operator==(const BridgeTxMintAuthorization& a,
                           const BridgeTxMintAuthorization& b)
    {
        return a.transaction_id == b.transaction_id &&
               a.output_index == b.output_index &&
               a.authorization.asset == b.authorization.asset &&
               a.authorization.amount == b.authorization.amount &&
               a.authorization.recipient_script ==
                   b.authorization.recipient_script &&
               a.authorization.nullifier == b.authorization.nullifier;
    }
};

/**
 * A managed-v1 reserve-release request created by one exact bUSD burn. The
 * stable base Txid is safe because its policy-9 output commits the type-10
 * record; witness re-signing therefore cannot create a second request id.
 */
struct BridgeManagedWithdrawalRequest {
    Txid transaction_id{};
    uint32_t burn_output_index{0};
    modern::AssetId asset{};
    CAmount amount{0};
    bridge::EthAddress ethereum_recipient{};
    int connected_height{-1};
    uint256 connected_block{};

    friend bool operator==(const BridgeManagedWithdrawalRequest&,
                           const BridgeManagedWithdrawalRequest&) = default;
};

struct BridgeWithdrawalId {
    Txid transaction_id{};
    uint32_t burn_output_index{0};

    friend bool operator==(const BridgeWithdrawalId&,
                           const BridgeWithdrawalId&) = default;
    friend bool operator<(const BridgeWithdrawalId& a,
                          const BridgeWithdrawalId& b)
    {
        if (a.transaction_id != b.transaction_id) {
            return a.transaction_id < b.transaction_id;
        }
        return a.burn_output_index < b.burn_output_index;
    }
};

/** The sole bridge semantic result one transaction can carry. */
struct BridgeTxAuthorization {
    std::optional<BridgeTxMintAuthorization> mint{};
    std::optional<BridgeManagedWithdrawalRequest> withdrawal{};
};

/** Exact before/after accounting for one configured asset epoch. */
struct BridgeEpochMintChange {
    modern::AssetId asset{};
    uint64_t epoch{0};
    CAmount before{0};
    CAmount after{0};

    friend bool operator==(const BridgeEpochMintChange&,
                           const BridgeEpochMintChange&) = default;
};

/**
 * Complete reversible effect of one active bridge block, including the prior
 * connected identity. Empty active blocks are retained so disconnect identity
 * remains exact. Light-client snapshots are present only when the block
 * changed that store.
 */
struct BridgeBlockDelta {
    int height{-1};
    uint256 block_hash{};
    int previous_height{-1};
    uint256 previous_block_hash{};
    std::optional<bridge::LightClientStore> light_client_before{};
    std::optional<bridge::LightClientStore> light_client_after{};
    std::vector<BridgeExecutionAnchor> anchors_added{};
    std::vector<BridgeTxMintAuthorization> mint_authorizations{};
    std::vector<bridge::BridgeDepositKey> nullifiers_added{};
    std::vector<BridgeEpochMintChange> epoch_mint_changes{};
    std::vector<BridgeManagedWithdrawalRequest> withdrawals_added{};
};

/**
 * Incremental, non-mutating bridge verification for one candidate block.
 *
 * The preview borrows the connected BridgeStateIndex as an immutable base and
 * owns only candidate-local overlays. TryAppend is atomic: a rejected chunk
 * leaves all previously accepted candidate effects intact.
 */
class BridgeBlockPreview
{
public:
    ~BridgeBlockPreview();

    BridgeBlockPreview(const BridgeBlockPreview&) = delete;
    BridgeBlockPreview& operator=(const BridgeBlockPreview&) = delete;

    bool TryAppend(std::span<const CTransactionRef> transactions,
                   std::string& error);

private:
    struct Impl;
    explicit BridgeBlockPreview(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;

    friend class BridgeStateIndex;
};

/**
 * Consensus bridge authorization state. Candidate verification operates on
 * base-state lookups plus per-candidate overlays and yields a complete delta;
 * live state changes only after that delta has been fully validated.
 */
class BridgeStateIndex
{
public:
    bool VerifyBlock(const CBlock& block, int height,
                     const uint256& block_hash,
                     const Consensus::Params& params,
                     BridgeBlockDelta& out, std::string& error) const;

    /** Mempool/miner preview against the currently connected state. */
    bool VerifyTransaction(const CTransaction& tx, int candidate_height,
                           int64_t candidate_time,
                           const Consensus::Params& params,
                           BridgeTxAuthorization& out,
                           std::string& error) const;

    /**
     * Begin an incremental block preview against this exact connected state.
     * The index must outlive the preview and remain unchanged while it is in
     * use (block assembly satisfies this while holding cs_main).
     */
    std::unique_ptr<BridgeBlockPreview> BeginBlockPreview(
        int candidate_height, int64_t candidate_time,
        const uint256& preview_block_id, const Consensus::Params& params,
        std::string& error) const;

    bool ConnectBlock(const BridgeBlockDelta& delta, std::string& error);
    bool DisconnectBlock(int height, const uint256& block_hash,
                         std::string& error);
    void Clear();

    bool IsNullified(const bridge::BridgeDepositKey& key) const
    {
        return m_nullifiers.contains(key);
    }
    std::optional<BridgeExecutionAnchor> Anchor(
        const uint256& block_hash) const;
    std::optional<BridgeManagedWithdrawalRequest> Withdrawal(
        const BridgeWithdrawalId& id) const;
    const std::optional<bridge::LightClientStore>& LightClient() const
    {
        return m_light_client;
    }
    CAmount EpochMinted(const modern::AssetId& asset, uint64_t epoch) const;
    int ConnectedHeight() const { return m_connected_height; }
    uint256 ConnectedHash() const { return m_connected_hash; }
    size_t NullifierCount() const { return m_nullifiers.size(); }
    size_t AnchorCount() const { return m_anchors.size(); }
    size_t WithdrawalCount() const { return m_withdrawals.size(); }
    const std::deque<BridgeBlockDelta>& History() const { return m_history; }

private:
    std::optional<bridge::LightClientStore> m_light_client{};
    std::map<uint256, BridgeExecutionAnchor> m_anchors{};
    std::map<uint64_t, uint256> m_anchor_by_height{};
    std::set<bridge::BridgeDepositKey> m_nullifiers{};
    std::map<std::pair<modern::AssetId, uint64_t>, CAmount> m_epoch_minted{};
    std::map<BridgeWithdrawalId, BridgeManagedWithdrawalRequest> m_withdrawals{};
    int m_connected_height{-1};
    uint256 m_connected_hash{};
    std::deque<BridgeBlockDelta> m_history{};
};

/**
 * Active-chain replay/undo driver. Restart and reindex correctness comes from
 * deterministic replay beginning at bridge activation. A production disk
 * sidecar must only replace this with a strict config-digest marker and one
 * synchronous state+undo batch; partial durability is intentionally absent.
 */
class BridgeStateTracker
{
public:
    bool Sync(const CChain& chain, const BlockManager& blockman,
              const Consensus::Params& params, const CBlockIndex& target);
    void BlockConnected(const CBlock& block, const CBlockIndex& index,
                        const Consensus::Params& params);
    void BlockDisconnected(const CBlockIndex& index,
                           const Consensus::Params& params);
    void MarkDirty() { m_dirty = true; }
    bool Synced(const uint256& tip_hash) const
    {
        return !m_dirty && m_synced_tip == tip_hash;
    }
    const BridgeStateIndex& Index() const { return m_index; }

private:
    bool ApplyBlock(const CBlock& block, const CBlockIndex& index,
                    const Consensus::Params& params);

    BridgeStateIndex m_index{};
    uint256 m_synced_tip{};
    int m_synced_height{-1};
    bool m_dirty{true};
};

} // namespace node

#endif // B3COIN_NODE_BRIDGE_STATE_H
