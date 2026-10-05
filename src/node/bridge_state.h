// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_BRIDGE_STATE_H
#define B3COIN_NODE_BRIDGE_STATE_H

#include <bridge/admission.h>
#include <bridge/eth_light_client.h>
#include <consensus/params.h>
#include <modern/withdrawal_tree.h>
#include <modern/withdrawal_v2.h>
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
class FinalityTracker;
struct LineageV2View;

/** Parent-derived ability of B3's current and next validator sets to produce
 * a withdrawal root accepted by the pinned Ethereum verifier. UNAVAILABLE is
 * operational (the parent finality state could not be reconstructed), while
 * NOT_READY is a deterministic consensus condition. */
enum class BridgeBurnReadiness {
    UNAVAILABLE,
    NOT_READY,
    READY,
};

//! True only for a syntactically decoded decentralized BRIDGE_BURN record.
bool HasDecentralizedBridgeBurn(const CTransaction& tx);
bool HasDecentralizedBridgeBurn(const CBlock& block);

/** Fail closed for a decoded BRIDGE_BURN unless parent/projected finality is
 * READY. Malformed type-10 records remain the bridge verifier's responsibility
 * and cannot pass it. */
bool CheckBridgeBurnReadiness(const CTransaction& tx,
                              BridgeBurnReadiness readiness,
                              std::string& error);
bool CheckBridgeBurnReadiness(const CBlock& block,
                              BridgeBurnReadiness readiness,
                              std::string& error);

/**
 * TEMPORARY bridge V2 burn gate (node changes section 8.4 rows C1/C2 only):
 * READY iff the V2 envelope is complete, the fresh lineage is configured,
 * bootstrapped and not broken, and its current and next committees each
 * have a member count within the Verifier V2 bounds and at least its
 * minimum total weight. Height >= W2 is the bridge verifier's rule (a kind 6
 * below W2 is invalid there). Never UNAVAILABLE: the caller reports
 * UNAVAILABLE when it cannot reconstruct the lineage view.
 * The kind 9 liveness rows (C3, C4, E1-E8), the impairment and wind-down
 * terms and the B-22 committee replace this predicate in later slices;
 * until then a V2 burn needs only a live fresh lineage. `reason` names the
 * first failed condition.
 */
BridgeBurnReadiness BridgeV2BurnReadiness(
    const LineageV2View& lineage, const Consensus::BridgeV2Params& params,
    std::string& reason);

/** CheckBridgeBurnReadiness for the V2 regime: the error text names the
 * temporary gate. Callers apply it only from the bridge V2 height. */
bool CheckBridgeV2BurnReadiness(const CTransaction& tx,
                                BridgeBurnReadiness readiness,
                                std::string& error);
bool CheckBridgeV2BurnReadiness(const CBlock& block,
                                BridgeBurnReadiness readiness,
                                std::string& error);

/**
 * The TEMPORARY bridge V2 burn gate for the block that would extend
 * `parent`: syncs `finality` to `parent` (with `bridge_index`, which must be
 * synced to `parent` too) and applies BridgeV2BurnReadiness to the fresh
 * lineage projected for parent + 1. UNAVAILABLE without a bridge V2
 * envelope or when the finality state cannot be reconstructed.
 *
 * The one evaluation shared by mempool admission, block connection
 * (validation.cpp), block assembly (node/miner.cpp) and wallet bridge
 * prevalidation (node/interfaces.cpp), so a V2 burn the assembler selects
 * is one the block rule accepts. W2 is checked by each caller (its own
 * refusal text); this function does not look at it. Under cs_main.
 */
BridgeBurnReadiness ProjectedBridgeV2BurnReadiness(
    FinalityTracker& finality, const CChain& chain,
    const BlockManager& blockman, const Consensus::Params& params,
    const CBlockIndex& parent, const BridgeStateIndex& bridge_index,
    std::string& reason);

/** Recent active-chain blocks whose bridge effects can be undone in memory. */
inline constexpr size_t BRIDGE_STATE_UNDO_BLOCKS{288};

/** A receipts-root anchor reached from the verified finalized Ethereum head. */
struct BridgeExecutionAnchor {
    uint64_t block_number{0};
    uint256 block_hash{};
    uint256 receipts_root{};
    uint64_t source_finalized_beacon_slot{0};
    uint64_t source_finalized_execution_block{0};
    uint64_t execution_timestamp{0};
    int connected_height{-1};
    uint256 connected_block{};

    friend bool operator==(const BridgeExecutionAnchor&,
                           const BridgeExecutionAnchor&) = default;
};

/** Active B3 block that connected one persistent bridge-state fact. */
struct BridgeStateConnection {
    int height{-1};
    uint256 block_hash{};

    friend bool operator==(const BridgeStateConnection&,
                           const BridgeStateConnection&) = default;
};

/**
 * One exact Ethereum light-client store and the active B3 block that made it
 * current. The bridge index retains one old floor snapshot in addition to its
 * bounded undo history so a stream of newer, not-yet-B3-finalized updates
 * cannot make every exportable snapshot disappear.
 */
struct BridgeLightClientSnapshot {
    bridge::LightClientStore store{};
    BridgeStateConnection connection{};
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

/**
 * One consensus-derived BRIDGE_BURN leaf. The record supplies only the
 * recipient and names the exact burn; every other Ethereum-facing field is
 * derived from pinned bridge parameters and active-chain position.
 */
struct BridgeDecentralizedWithdrawalRequest {
    Txid transaction_id{};
    uint32_t burn_output_index{0};
    modern::BridgeWithdrawalV1 withdrawal{};
    uint256 leaf{};
    int connected_height{-1};
    uint256 connected_block{};

    friend bool operator==(const BridgeDecentralizedWithdrawalRequest&,
                           const BridgeDecentralizedWithdrawalRequest&) = default;
};

/**
 * One consensus-derived bridge V2 leaf (node changes sections 7.2-7.3). As
 * for V1, the kind 6 record supplies only the recipient and names the exact
 * bUSD V2 burn; the id, destination chain, asset, vault, token and B3 height
 * come from the V2 entry and the active-chain position.
 */
struct BridgeWithdrawalV2Request {
    Txid transaction_id{};
    uint32_t burn_output_index{0};
    modern::BridgeWithdrawalV2 withdrawal{};
    uint256 leaf{};
    int connected_height{-1};
    uint256 connected_block{};

    friend bool operator==(const BridgeWithdrawalV2Request&,
                           const BridgeWithdrawalV2Request&) = default;
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
    std::optional<BridgeDecentralizedWithdrawalRequest>
        decentralized_withdrawal{};
    std::optional<BridgeWithdrawalV2Request> withdrawal_v2{};

    bool HasWithdrawal() const
    {
        return withdrawal.has_value() ||
               decentralized_withdrawal.has_value() ||
               withdrawal_v2.has_value();
    }
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
 *
 * From the bridge V2 activation height V (`v2_store`), the light-client
 * fields and `anchors_added` refer to the separate V2 store, mints use
 * nullifier namespace 2, the V1 withdrawal tree is frozen and V2 burns
 * append to the V2 tree (node changes section 3.4). The V2 tree fields are
 * carried at every height; below V they stay the empty tree.
 */
struct BridgeBlockDelta {
    int height{-1};
    uint256 block_hash{};
    int previous_height{-1};
    uint256 previous_block_hash{};
    std::optional<bridge::LightClientStore> light_client_before{};
    std::optional<bridge::LightClientStore> light_client_after{};
    std::optional<BridgeStateConnection> light_client_connection_before{};
    std::optional<BridgeStateConnection> light_client_connection_after{};
    std::vector<BridgeExecutionAnchor> anchors_added{};
    std::vector<BridgeTxMintAuthorization> mint_authorizations{};
    std::vector<bridge::BridgeDepositKey> nullifiers_added{};
    std::vector<BridgeEpochMintChange> epoch_mint_changes{};
    std::vector<BridgeManagedWithdrawalRequest> withdrawals_added{};
    std::vector<BridgeDecentralizedWithdrawalRequest>
        decentralized_withdrawals_added{};
    modern::WithdrawalTreeState withdrawal_tree_before{};
    modern::WithdrawalTreeState withdrawal_tree_after{};
    bool v2_store{false};
    std::vector<BridgeWithdrawalV2Request> withdrawals_v2_added{};
    modern::WithdrawalTreeState withdrawal_tree_v2_before{};
    modern::WithdrawalTreeState withdrawal_tree_v2_after{};
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
    std::optional<BridgeStateConnection> NullifierConnection(
        const bridge::BridgeDepositKey& key) const;
    std::optional<BridgeExecutionAnchor> Anchor(
        const uint256& block_hash) const;
    /**
     * Nearest retained execution anchor at or after `target_block_number`
     * whose B3 connection is covered by `finalized_b3_height` and whose
     * directly-finalized Ethereum origin can still reach the target within
     * the cumulative backfill limit.
     *
     * The index contains active-chain state only. Callers exposing this
     * across a trust boundary must additionally match connected_block to the
     * active B3 chain at connected_height.
     */
    std::optional<BridgeExecutionAnchor> AnchorForTarget(
        uint64_t target_block_number, int finalized_b3_height) const;
    std::optional<BridgeManagedWithdrawalRequest> Withdrawal(
        const BridgeWithdrawalId& id) const;
    std::optional<BridgeDecentralizedWithdrawalRequest>
    DecentralizedWithdrawal(uint64_t withdrawal_id) const;
    /**
     * Resolve a confirmed active-chain burn source to its consensus-assigned
     * withdrawal. Unlike a mempool preview, this identity survives unrelated
     * transaction ordering and disappears automatically on active-chain undo.
     */
    std::optional<BridgeDecentralizedWithdrawalRequest>
    DecentralizedWithdrawal(const BridgeWithdrawalId& source) const;
    /** Exact consecutive withdrawal prefix connected through `height`. */
    std::optional<std::vector<BridgeDecentralizedWithdrawalRequest>>
    DecentralizedWithdrawalsThrough(int height) const;
    std::optional<uint256> WithdrawalRootAtHeight(int height) const;
    const modern::WithdrawalTreeState& WithdrawalTree() const
    {
        return m_withdrawal_tree;
    }
    const std::optional<bridge::LightClientStore>& LightClient() const
    {
        return m_light_client;
    }
    const std::optional<BridgeStateConnection>&
    LightClientLastConnected() const
    {
        return m_light_client_connection;
    }
    /**
     * Newest retained store whose connection height is covered by B3
     * finality. Recent candidates come from the bounded undo history; the
     * first active-chain store is retained as a starvation-proof floor.
     * Callers must still verify connection.block_hash against the active chain.
     * This is the V1 store; V2-store deltas are skipped.
     */
    std::optional<BridgeLightClientSnapshot>
    FinalizedLightClientSnapshot(int finalized_b3_height) const;
    CAmount EpochMinted(const modern::AssetId& asset, uint64_t epoch) const;
    int ConnectedHeight() const { return m_connected_height; }
    uint256 ConnectedHash() const { return m_connected_hash; }
    //! Every nullifier, both namespaces.
    size_t NullifierCount() const { return m_nullifiers.size(); }
    //! Nullifiers of one namespace (bridge::BRIDGE_NULLIFIER_NAMESPACE_*).
    size_t NullifierCount(uint8_t nullifier_namespace) const;
    size_t AnchorCount() const { return m_anchors.size(); }
    size_t WithdrawalCount() const { return m_withdrawals.size(); }
    size_t DecentralizedWithdrawalCount() const
    {
        return m_decentralized_withdrawals.size();
    }
    const std::deque<BridgeBlockDelta>& History() const { return m_history; }

    // -- Bridge V2 (node changes sections 3.4, 4 and 7) -------------------
    // The V2 light-client store and anchors start empty at V; the V1 store,
    // nullifiers and tree freeze at V. Withdrawal roots V2 are recorded for
    // every connected height from V only; below V (and on every chain
    // without a bridge V2 envelope) FinalityWithdrawalRootV2 is zero without
    // consulting the index.

    const std::optional<bridge::LightClientStore>& LightClientV2() const
    {
        return m_light_client_v2;
    }
    const std::optional<BridgeStateConnection>&
    LightClientV2LastConnected() const
    {
        return m_light_client_connection_v2;
    }
    //! FinalizedLightClientSnapshot over the V2 store only.
    std::optional<BridgeLightClientSnapshot>
    FinalizedLightClientSnapshotV2(int finalized_b3_height) const;
    std::optional<BridgeExecutionAnchor> AnchorV2(
        const uint256& block_hash) const;
    //! AnchorForTarget over the V2 anchors (same rules and caveats).
    std::optional<BridgeExecutionAnchor> AnchorForTargetV2(
        uint64_t target_block_number, int finalized_b3_height) const;
    size_t AnchorCountV2() const { return m_anchors_v2.size(); }
    std::optional<BridgeWithdrawalV2Request> WithdrawalV2(
        uint64_t withdrawal_id) const;
    //! Confirmed active-chain V2 burn source -> its consensus leaf.
    std::optional<BridgeWithdrawalV2Request> WithdrawalV2(
        const BridgeWithdrawalId& source) const;
    //! Exact consecutive V2 withdrawal prefix connected through `height`.
    std::optional<std::vector<BridgeWithdrawalV2Request>>
    WithdrawalsV2Through(int height) const;
    std::optional<uint256> WithdrawalRootV2AtHeight(int height) const;
    const modern::WithdrawalTreeState& WithdrawalTreeV2() const
    {
        return m_withdrawal_tree_v2;
    }
    size_t WithdrawalV2Count() const { return m_withdrawals_v2.size(); }

private:
    std::optional<BridgeLightClientSnapshot> FinalizedSnapshot(
        int finalized_b3_height, bool v2_store) const;

    std::optional<bridge::LightClientStore> m_light_client{};
    std::optional<BridgeStateConnection> m_light_client_connection{};
    std::optional<BridgeLightClientSnapshot> m_light_client_floor{};
    std::map<uint256, BridgeExecutionAnchor> m_anchors{};
    std::map<uint64_t, uint256> m_anchor_by_height{};
    std::set<bridge::BridgeDepositKey> m_nullifiers{};
    std::map<bridge::BridgeDepositKey, BridgeStateConnection>
        m_nullifier_connections{};
    std::map<std::pair<modern::AssetId, uint64_t>, CAmount> m_epoch_minted{};
    std::map<BridgeWithdrawalId, BridgeManagedWithdrawalRequest> m_withdrawals{};
    std::map<uint64_t, BridgeDecentralizedWithdrawalRequest>
        m_decentralized_withdrawals{};
    std::map<BridgeWithdrawalId, uint64_t> m_decentralized_withdrawal_sources{};
    modern::WithdrawalTreeState m_withdrawal_tree{};
    std::map<int, uint256> m_withdrawal_roots{};
    std::optional<bridge::LightClientStore> m_light_client_v2{};
    std::optional<BridgeStateConnection> m_light_client_connection_v2{};
    std::optional<BridgeLightClientSnapshot> m_light_client_floor_v2{};
    std::map<uint256, BridgeExecutionAnchor> m_anchors_v2{};
    std::map<uint64_t, uint256> m_anchor_by_height_v2{};
    std::map<uint64_t, BridgeWithdrawalV2Request> m_withdrawals_v2{};
    std::map<BridgeWithdrawalId, uint64_t> m_withdrawal_v2_sources{};
    modern::WithdrawalTreeState m_withdrawal_tree_v2{};
    std::map<int, uint256> m_withdrawal_v2_roots{};
    int m_connected_height{-1};
    uint256 m_connected_hash{};
    std::deque<BridgeBlockDelta> m_history{};
};

/**
 * Exact withdrawal root a finality certificate must carry at `height`.
 * Managed or pre-inbound-B epochs use zero. From inbound B onward a
 * decentralized bridge requires the indexed canonical cumulative root even
 * while the separate irreversible-burn height W remains unset; the empty
 * depth-32 tree root is nonzero. An unavailable index fails closed.
 */
std::optional<uint256> FinalityWithdrawalRoot(
    int height, const Consensus::Params& params,
    const BridgeStateIndex* bridge_index);

/**
 * withdrawalRoot a fresh-lineage (digest V2) certificate must carry at
 * `height` (node changes section 7.4; contracts spec section 2.4): zero below
 * V or without a complete bridge V2 envelope, from V the indexed V2 tree root
 * (the empty depth-32 root is non-zero). An index lacking the height fails
 * closed, as does a missing index from V.
 */
std::optional<uint256> FinalityWithdrawalRootV2(
    int height, const Consensus::Params& params,
    const BridgeStateIndex* bridge_index);

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
