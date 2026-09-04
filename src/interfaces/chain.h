// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_CHAIN_H
#define BITCOIN_INTERFACES_CHAIN_H

#include <blockfilter.h>
#include <common/settings.h>
#include <crypto/bls.h>
#include <flowmesh/batch.h>
#include <uint256.h>
#include <script/script.h>
#include <key.h>
#include <kernel/chain.h> // IWYU pragma: export
#include <modern/flowmesh_checkpoint.h>
#include <node/types.h>
#include <primitives/transaction.h>
#include <util/result.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ArgsManager;
class CBlock;
class CBlockUndo;
class CFeeRate;
class CRPCCommand;
class CScheduler;
class Coin;
class uint256;
enum class MemPoolRemovalReason;
enum class RBFTransactionState;
struct bilingual_str;
struct CBlockLocator;
struct FeeCalculation;
namespace kernel {
struct ChainstateRole;
} // namespace kernel
namespace node {
struct BridgeTxAuthorization;
struct NodeContext;
} // namespace node

namespace interfaces {

class Handler;
class Wallet;

//! Helper for findBlock to selectively return pieces of block data. If block is
//! found, data will be returned by setting specified output variables. If block
//! is not found, output variables will keep their previous values.
class FoundBlock
{
public:
    FoundBlock& hash(uint256& hash) { m_hash = &hash; return *this; }
    FoundBlock& height(int& height) { m_height = &height; return *this; }
    FoundBlock& time(int64_t& time) { m_time = &time; return *this; }
    FoundBlock& maxTime(int64_t& max_time) { m_max_time = &max_time; return *this; }
    FoundBlock& mtpTime(int64_t& mtp_time) { m_mtp_time = &mtp_time; return *this; }
    //! Return whether block is in the active (most-work) chain.
    FoundBlock& inActiveChain(bool& in_active_chain) { m_in_active_chain = &in_active_chain; return *this; }
    //! Return locator if block is in the active chain.
    FoundBlock& locator(CBlockLocator& locator) { m_locator = &locator; return *this; }
    //! Return next block in the active chain if current block is in the active chain.
    FoundBlock& nextBlock(const FoundBlock& next_block) { m_next_block = &next_block; return *this; }
    //! Read block data from disk. If the block exists but doesn't have data
    //! (for example due to pruning), the CBlock variable will be set to null.
    FoundBlock& data(CBlock& data) { m_data = &data; return *this; }

    uint256* m_hash = nullptr;
    int* m_height = nullptr;
    int64_t* m_time = nullptr;
    int64_t* m_max_time = nullptr;
    int64_t* m_mtp_time = nullptr;
    bool* m_in_active_chain = nullptr;
    CBlockLocator* m_locator = nullptr;
    const FoundBlock* m_next_block = nullptr;
    CBlock* m_data = nullptr;
    mutable bool found = false;
};

//! The action to be taken after updating a settings value.
//! WRITE indicates that the updated value must be written to disk,
//! while SKIP_WRITE indicates that the change will be kept in memory-only
//! without persisting it.
enum class SettingsAction {
    WRITE,
    SKIP_WRITE
};

using SettingsUpdate = std::function<std::optional<interfaces::SettingsAction>(common::SettingsValue&)>;

//! B3 modern-era finality status (see Chain::finalityStatus): the epoch
//! state machine, the finalized checkpoint and persisted pin, the local
//! signature pool, and -- when a validator key is given -- its FINALITY_KEY
//! binding and set membership. Reusable by RPC, Qt and other clients.
struct FinalityStatus {
    //! Modern-PoS rules configured and the legacy boundary pinned.
    bool configured{false};
    //! The chain has reached the modern-PoS phase and the state is derivable.
    bool active{false};
    uint256 chain_domain{};
    bool bootstrapped{false};
    uint64_t epoch{0};
    int epoch_start{-1};
    bool handover_certified{false};
    bool lineage_broken{false};
    int set_size{0};
    uint64_t total_weight{0};
    uint64_t quorum_weight{0};
    uint256 current_set_hash{};
    uint256 next_set_hash{};
    std::optional<int> finalized_height;
    uint256 finalized_hash{};
    uint64_t finalized_epoch{0};
    std::optional<int> pin_height;
    uint256 pin_hash{};
    uint64_t pool_checkpoints{0};
    // ---- one-time Ethereum bridge bootstrap handoff --------------------
    // Retained after M because this is public consensus data and the
    // Ethereum deployment window need not match B3's certificate window.
    std::optional<int> bootstrap_snapshot_height;
    uint256 bootstrap_snapshot_hash{};
    uint256 bootstrap_set_hash{};
    std::vector<unsigned char> bootstrap_set_header;
    // ---- per-validator (present when a validator key was given) ----
    bool bound{false};
    bool revoked{false};
    uint32_t binding_seq{0};
    std::vector<unsigned char> binding_bls_pubkey;
    int binding_height{-1};
    bool in_current_set{false};
    uint64_t member_weight{0};
};

//! B3 Modern PoS staking status (see Chain::stakingStatus).
struct StakingStatus {
    //! A staking loop exists in this node.
    bool available{false};
    bool running{false};
    //! Human-readable loop state ("producing", "waiting: ...", "stopped").
    std::string state;
    std::string last_error;
    //! The loop's validator key (x-only), when running.
    std::optional<std::array<unsigned char, 32>> validator_key;
    int64_t blocks_produced{0};
    uint256 last_block_hash;
    //! The forced timestamp of the next block this validator may produce (0 if unknown).
    int64_t next_block_time{0};
    //! Chain facts at the time of the call.
    int tip_height{-1};
    //! "legacy" | "corridor" | "modern_pos" for the NEXT block (B3 chains); "modern" otherwise.
    std::string next_block_phase;
    //! Modern PoS is configured and the next block is a modern-PoS block.
    bool modern_pos_active{false};
    //! Epoch-frozen block-production weight of the selected validator key and
    //! total weight of the validator set in force at the next height, in whole
    //! modern B3 (one stake universe: the same snapshot numbers that define
    //! the finality quorum). Once an epoch set is in force, newly ACTIVE stake
    //! enters only at a later certified rotation; it never mutates that set
    //! mid-epoch.
    CAmount active_weight{0};
    CAmount total_active_weight{0};
    std::optional<CAmount> min_stake_amount;
    int stake_activation_depth{0};
    //! Finality signing (Commit 16): whether a BLS consensus key is armed in
    //! the staking loop, and the highest checkpoint it has signed.
    bool finality_signing{false};
    int last_signed_height{-1};
};

//! One production FlowMesh market plus an optional wallet-account view.
//! The node owns the authoritative runtime/state; this value is a bounded
//! copy for wallet RPC and GUI clients.
struct FlowMeshMarketStatus {
    bool available{false};
    bool running{false};
    uint256 domain{};
    uint256 market_id{};
    uint256 vault_id{};
    uint256 base_asset{};
    uint256 quote_asset{};
    uint256 execution_config_id{};
    uint64_t epoch{0};
    uint64_t next_microblock_sequence{0};
    uint64_t next_effect_index{0};
    uint32_t round{0};
    uint256 last_microblock_hash{};
    uint256 state_root{};
    size_t pending_actions{0};
    bool observer_only{true};
    bool paused{false};
    bool pending_handoff{false};
    bool checkpoint_pending{false};
    uint256 pending_checkpoint_id{};
    uint64_t pending_checkpoint_sequence{0};
    uint32_t pending_checkpoint_effect_count{0};
    std::string halt{"unavailable"};
    std::string error;
    std::optional<uint256> account_id;
    uint64_t next_account_sequence{0};
    uint64_t slot{0};
    CAmount base_available{0};
    CAmount base_reserved{0};
    CAmount b3_available{0};
    CAmount b3_reserved{0};
};

//! Fully encoded, service-selected type-8 record. Bitmap sizing and the
//! historical seat-set lookup stay inside the node service.
struct FlowMeshPendingCheckpoint {
    CMpaRecord record;
    uint256 checkpoint_id{};
    uint64_t sequence{0};
    uint32_t effect_count{0};
};

//! One exact live keyless vault input selected by the node service for a
//! certified type-9 operation. The wallet must preserve this outpoint/output
//! pair and leave its scriptSig and witness empty.
struct FlowMeshVaultInput {
    COutPoint outpoint;
    CTxOut txout;
};

//! Wallet-ready certified vault operation. The service resolves the connected
//! checkpoint proof and deterministic custody inputs; the wallet contributes
//! only the exact payout/change outputs and a separately signed native fee
//! input.
struct FlowMeshVaultOperation {
    uint256 market_id{};
    uint256 checkpoint_id{};
    CMpaRecord record;
    modern::FlowMeshEffectV1 effect;
    std::vector<FlowMeshVaultInput> inputs;
};

//! Atomic node snapshot used while wallet RPCs construct modern creation
//! transactions. Keeping this behind Chain avoids treating a wallet RPC's
//! WalletContext as a NodeContext.
struct ModernCreationSnapshot {
    uint256 tip_hash{};
    int next_height{0};
    std::optional<uint32_t> fn_issued_before{};
    bool pending_fn_pod{false};
};

//! Result category for node-owned bridge transaction prevalidation. Wallet
//! clients use this to preserve the RPC error class without reaching through
//! the Chain interface into validation and bridge-index internals.
enum class BridgePrevalidationResult {
    VALID,
    TIP_CHANGED,
    RULES_INACTIVE,
    STATE_UNAVAILABLE,
    REJECTED,
};

//! Interface giving clients (wallet processes, maybe other analysis tools in
//! the future) ability to access to the chain state, receive notifications,
//! estimate fees, and submit transactions.
//!
//! TODO: Current chain methods are too low level, exposing too much of the
//! internal workings of the bitcoin node, and not being very convenient to use.
//! Chain methods should be cleaned up and simplified over time. Examples:
//!
//! * The initMessages() and showProgress() methods which the wallet uses to send
//!   notifications to the GUI should go away when GUI and wallet can directly
//!   communicate with each other without going through the node
//!   (https://github.com/bitcoin/bitcoin/pull/15288#discussion_r253321096).
//!
//! * The handleRpc, registerRpcs, rpcEnableDeprecated methods and other RPC
//!   methods can go away if wallets listen for HTTP requests on their own
//!   ports instead of registering to handle requests on the node HTTP port.
//!
//! * Move fee estimation queries to an asynchronous interface and let the
//!   wallet cache it, fee estimation being driven by node mempool, wallet
//!   should be the consumer.
//!
//! * `guessVerificationProgress` and similar methods can go away if rescan
//!   logic moves out of the wallet, and the wallet just requests scans from the
//!   node (https://github.com/bitcoin/bitcoin/issues/11756)
class Chain
{
public:
    virtual ~Chain() = default;

    //! Get current chain height, not including genesis block (returns 0 if
    //! chain only contains genesis block, nullopt if chain does not contain
    //! any blocks)
    virtual std::optional<int> getHeight() = 0;

    //! Get block hash. Height must be valid or this function will abort.
    virtual uint256 getBlockHash(int height) = 0;

    //! Check that the block is available on disk (i.e. has not been
    //! pruned), and contains transactions.
    virtual bool haveBlockOnDisk(int height) = 0;

    //! Return height of the highest block on chain in common with the locator,
    //! which will either be the original block used to create the locator,
    //! or one of its ancestors.
    virtual std::optional<int> findLocatorFork(const CBlockLocator& locator) = 0;

    //! Returns whether a block filter index is available.
    virtual bool hasBlockFilterIndex(BlockFilterType filter_type) = 0;

    //! Returns whether any of the elements match the block via a BIP 157 block filter
    //! or std::nullopt if the block filter for this block couldn't be found.
    virtual std::optional<bool> blockFilterMatchesAny(BlockFilterType filter_type, const uint256& block_hash, const GCSFilter::ElementSet& filter_set) = 0;

    //! Return whether node has the block and optionally return block metadata
    //! or contents.
    virtual bool findBlock(const uint256& hash, const FoundBlock& block={}) = 0;

    //! Find first block in the chain with timestamp >= the given time
    //! and height >= than the given height, return false if there is no block
    //! with a high enough timestamp and height. Optionally return block
    //! information.
    virtual bool findFirstBlockWithTimeAndHeight(int64_t min_time, int min_height, const FoundBlock& block={}) = 0;

    //! Find ancestor of block at specified height and optionally return
    //! ancestor information.
    virtual bool findAncestorByHeight(const uint256& block_hash, int ancestor_height, const FoundBlock& ancestor_out={}) = 0;

    //! Return whether block descends from a specified ancestor, and
    //! optionally return ancestor information.
    virtual bool findAncestorByHash(const uint256& block_hash,
        const uint256& ancestor_hash,
        const FoundBlock& ancestor_out={}) = 0;

    //! Find most recent common ancestor between two blocks and optionally
    //! return block information.
    virtual bool findCommonAncestor(const uint256& block_hash1,
        const uint256& block_hash2,
        const FoundBlock& ancestor_out={},
        const FoundBlock& block1_out={},
        const FoundBlock& block2_out={}) = 0;

    //! Look up unspent output information. Returns coins in the mempool and in
    //! the current chain UTXO set. Iterates through all the keys in the map and
    //! populates the values.
    virtual void findCoins(std::map<COutPoint, Coin>& coins) = 0;

    //! Estimate fraction of total transactions verified if blocks up to
    //! the specified block hash are verified.
    virtual double guessVerificationProgress(const uint256& block_hash) = 0;

    //! Return true if data is available for all blocks in the specified range
    //! of blocks. This checks all blocks that are ancestors of block_hash in
    //! the height range from min_height to max_height, inclusive.
    virtual bool hasBlocks(const uint256& block_hash, int min_height = 0, std::optional<int> max_height = {}) = 0;

    //! Check if transaction is RBF opt in.
    virtual RBFTransactionState isRBFOptIn(const CTransaction& tx) = 0;

    //! Check if transaction is in mempool.
    virtual bool isInMempool(const Txid& txid) = 0;

    //! Check if transaction has descendants in mempool.
    virtual bool hasDescendantsInMempool(const Txid& txid) = 0;

    //! Process a local transaction, optionally adding it to the mempool and
    //! optionally broadcasting it to the network.
    //! @param[in] tx Transaction to process.
    //! @param[in] max_tx_fee Don't add the transaction to the mempool or
    //! broadcast it if its fee is higher than this.
    //! @param[in] broadcast_method Whether to add the transaction to the
    //! mempool and how/whether to broadcast it.
    //! @param[out] err_string Set if an error occurs.
    //! @return False if the transaction could not be added due to the fee or for another reason.
    virtual bool broadcastTransaction(const CTransactionRef& tx,
                                      const CAmount& max_tx_fee,
                                      node::TxBroadcast broadcast_method,
                                      std::string& err_string) = 0;

    //! Calculate mempool ancestor and cluster counts for the given transaction.
    virtual void getTransactionAncestry(const Txid& txid, size_t& ancestors, size_t& cluster_count, size_t* ancestorsize = nullptr, CAmount* ancestorfees = nullptr) = 0;

    //! For each outpoint, calculate the fee-bumping cost to spend this outpoint at the specified
    //  feerate, including bumping its ancestors. For example, if the target feerate is 10sat/vbyte
    //  and this outpoint refers to a mempool transaction at 3sat/vbyte, the bump fee includes the
    //  cost to bump the mempool transaction to 10sat/vbyte (i.e. 7 * mempooltx.vsize). If that
    //  transaction also has, say, an unconfirmed parent with a feerate of 1sat/vbyte, the bump fee
    //  includes the cost to bump the parent (i.e. 9 * parentmempooltx.vsize).
    //
    //  If the outpoint comes from an unconfirmed transaction that is already above the target
    //  feerate or bumped by its descendant(s) already, it does not need to be bumped. Its bump fee
    //  is 0. Likewise, if any of the transaction's ancestors are already bumped by a transaction
    //  in our mempool, they are not included in the transaction's bump fee.
    //
    //  Also supported is bump-fee calculation in the case of replacements. If an outpoint
    //  conflicts with another transaction in the mempool, it is assumed that the goal is to replace
    //  that transaction. As such, the calculation will exclude the to-be-replaced transaction, but
    //  will include the fee-bumping cost. If bump fees of descendants of the to-be-replaced
    //  transaction are requested, the value will be 0. Fee-related RBF rules are not included as
    //  they are logically distinct.
    //
    //  Any outpoints that are otherwise unavailable from the mempool (e.g. UTXOs from confirmed
    //  transactions or transactions not yet broadcast by the wallet) are given a bump fee of 0.
    //
    //  If multiple outpoints come from the same transaction (which would be very rare because
    //  it means that one transaction has multiple change outputs or paid the same wallet using multiple
    //  outputs in the same transaction) or have shared ancestry, the bump fees are calculated
    //  independently, i.e. as if only one of them is spent. This may result in double-fee-bumping. This
    //  caveat can be rectified per use of the sister-function CalculateCombinedBumpFee(…).
    virtual std::map<COutPoint, CAmount> calculateIndividualBumpFees(const std::vector<COutPoint>& outpoints, const CFeeRate& target_feerate) = 0;

    //! Calculate the combined bump fee for an input set per the same strategy
    //  as in CalculateIndividualBumpFees(…).
    //  Unlike CalculateIndividualBumpFees(…), this does not return individual
    //  bump fees per outpoint, but a single bump fee for the shared ancestry.
    //  The combined bump fee may be used to correct overestimation due to
    //  shared ancestry by multiple UTXOs after coin selection.
    virtual std::optional<CAmount> calculateCombinedBumpFee(const std::vector<COutPoint>& outpoints, const CFeeRate& target_feerate) = 0;

    //! Get the node's package limits.
    //! Currently only returns the ancestor and descendant count limits, but could be enhanced to
    //! return more policy settings.
    virtual void getPackageLimits(unsigned int& limit_ancestor_count, unsigned int& limit_descendant_count) = 0;

    //! Check if transaction will pass the mempool's chain limits.
    virtual util::Result<void> checkChainLimits(const CTransactionRef& tx) = 0;

    //! Estimate smart fee.
    virtual CFeeRate estimateSmartFee(int num_blocks, bool conservative, FeeCalculation* calc = nullptr) = 0;

    //! Fee estimator max target.
    virtual unsigned int estimateMaxBlocks() = 0;

    //! Mempool minimum fee.
    virtual CFeeRate mempoolMinFee() = 0;

    //! Relay current minimum fee (from -minrelaytxfee and -incrementalrelayfee settings).
    virtual CFeeRate relayMinFee() = 0;

    //! Relay incremental fee setting (-incrementalrelayfee), reflecting cost of relay.
    virtual CFeeRate relayIncrementalFee() = 0;

    //! Relay dust fee setting (-dustrelayfee), reflecting lowest rate it's economical to spend.
    virtual CFeeRate relayDustFee() = 0;

    //! Check if any block has been pruned.
    virtual bool havePruned() = 0;

    //! Get the current prune height.
    virtual std::optional<int> getPruneHeight() = 0;

    //! Check if the node is ready to broadcast transactions.
    virtual bool isReadyToBroadcast() = 0;

    //! Check if in IBD.
    virtual bool isInitialBlockDownload() = 0;

    //! Check if shutdown requested.
    virtual bool shutdownRequested() = 0;

    //! Send init message.
    virtual void initMessage(const std::string& message) = 0;

    //! Send init warning.
    virtual void initWarning(const bilingual_str& message) = 0;

    //! Send init error.
    virtual void initError(const bilingual_str& message) = 0;

    //! Send progress indicator.
    virtual void showProgress(const std::string& title, int progress, bool resume_possible) = 0;

    //! Chain notifications.
    class Notifications
    {
    public:
        virtual ~Notifications() = default;
        virtual void transactionAddedToMempool(const CTransactionRef& tx) {}
        virtual void transactionRemovedFromMempool(const CTransactionRef& tx, MemPoolRemovalReason reason) {}
        virtual void blockConnected(const kernel::ChainstateRole& role, const BlockInfo& block) {}
        virtual void blockDisconnected(const BlockInfo& block) {}
        virtual void updatedBlockTip() {}
        virtual void chainStateFlushed(const kernel::ChainstateRole& role, const CBlockLocator& locator) {}
    };

    //! Options specifying which chain notifications are required.
    struct NotifyOptions
    {
        //! Include undo data with block connected notifications.
        bool connect_undo_data = false;
        //! Include block data with block disconnected notifications.
        bool disconnect_data = false;
        //! Include undo data with block disconnected notifications.
        bool disconnect_undo_data = false;
    };

    //! Register handler for notifications.
    //! Some notifications are asynchronous and may still execute after the handler is disconnected.
    //! Use waitForNotifications() after the handler is disconnected to ensure all pending notifications
    //! have been processed.
    virtual std::unique_ptr<Handler> handleNotifications(std::shared_ptr<Notifications> notifications) = 0;

    //! Wait for pending notifications to be processed unless block hash points to the current
    //! chain tip.
    virtual void waitForNotificationsIfTipChanged(const uint256& old_tip) = 0;

    //! Wait for all pending notifications up to this point to be processed
    virtual void waitForNotifications() = 0;

    //! Register handler for RPC. Command is not copied, so reference
    //! needs to remain valid until Handler is disconnected.
    virtual std::unique_ptr<Handler> handleRpc(const CRPCCommand& command) = 0;

    //! Check if deprecated RPC is enabled.
    virtual bool rpcEnableDeprecated(const std::string& method) = 0;

    //! Get settings value.
    virtual common::SettingsValue getSetting(const std::string& arg) = 0;

    //! Get list of settings values.
    virtual std::vector<common::SettingsValue> getSettingsList(const std::string& arg) = 0;

    //! Return <datadir>/settings.json setting value.
    virtual common::SettingsValue getRwSetting(const std::string& name) = 0;

    //! Updates a setting in <datadir>/settings.json.
    //! Null can be passed to erase the setting. There is intentionally no
    //! support for writing null values to settings.json.
    //! Depending on the action returned by the update function, this will either
    //! update the setting in memory or write the updated settings to disk.
    virtual bool updateRwSetting(const std::string& name, const SettingsUpdate& update_function) = 0;

    //! Replace a setting in <datadir>/settings.json with a new value.
    //! Null can be passed to erase the setting.
    //! This method provides a simpler alternative to updateRwSetting when
    //! atomically reading and updating the setting is not required.
    virtual bool overwriteRwSetting(const std::string& name, common::SettingsValue value, SettingsAction action = SettingsAction::WRITE) = 0;

    //! Delete a given setting in <datadir>/settings.json.
    //! This method provides a simpler alternative to overwriteRwSetting when
    //! erasing a setting, for ease of use and readability.
    virtual bool deleteRwSettings(const std::string& name, SettingsAction action = SettingsAction::WRITE) = 0;

    //! Synchronously send transactionAddedToMempool notifications about all
    //! current mempool transactions to the specified handler and return after
    //! the last one is sent. These notifications aren't coordinated with async
    //! notifications sent by handleNotifications, so out of date async
    //! notifications from handleNotifications can arrive during and after
    //! synchronous notifications from requestMempoolTransactions. Clients need
    //! to be prepared to handle this by ignoring notifications about unknown
    //! removed transactions and already added new transactions.
    virtual void requestMempoolTransactions(Notifications& notifications) = 0;

    //! Return one chain+mempool-consistent snapshot for asset/FN/FlowMesh
    //! wallet construction. When inspect_fn_pool is false the FN fields are
    //! intentionally omitted.
    virtual std::optional<ModernCreationSnapshot> modernCreationSnapshot(
        bool inspect_fn_pool, std::string& error) = 0;

    //! Validate one candidate bridge transaction against the exact active tip
    //! used to construct it. The full node owns bridge-index synchronization;
    //! wallet code receives only the semantic authorization and result class.
    virtual BridgePrevalidationResult prevalidateBridgeTransaction(
        const CTransaction& tx, const uint256& expected_tip_hash,
        int expected_next_height, node::BridgeTxAuthorization& authorization,
        std::string& error) = 0;

    //! Return true if an assumed-valid snapshot is in use. Note that this
    //! returns true even after the snapshot is validated, until the next node
    //! restart.
    virtual bool hasAssumedValidChain() = 0;

    //! B3 Modern PoS staking (release-v1 validator UX, owner ruling
    //! 2026-08-23): start the node's automatic staking loop with the wallet's
    //! validator secret key and the script that receives block fees.
    virtual bool startStaking(const CKey& validator_key, const CScript& coinbase_script,
                              const std::optional<bls::SecretKey>& finality_key,
                              std::string& error) = 0;
    //! Stop the staking loop (no-op if not running).
    virtual void stopStaking() = 0;
    //! Staking status; `validator_key` (x-only) selects whose stake weight to
    //! report when the loop is not running (the loop's own key otherwise).
    virtual StakingStatus stakingStatus(const std::optional<std::array<unsigned char, 32>>& validator_key) = 0;

    //! B3: finality diagnostics (epoch state, finalized checkpoint, pin,
    //! signature pool; binding/membership of `validator_key` when given).
    virtual FinalityStatus finalityStatus(const std::optional<std::array<unsigned char, 32>>& validator_key) = 0;
    //! B3: arm the staking loop's finality signer with a BLS consensus key
    //! (refused while the loop runs; the key stays in node memory only).
    virtual bool armFinalitySigner(const bls::SecretKey& key, const std::array<unsigned char, 32>& validator_key, std::string& error) = 0;
    //! B3: drop the armed finality key (refused while the loop runs).
    virtual bool disarmFinalitySigner(std::string& error) = 0;

    //! B3 FlowMesh production runtime and wallet-action boundary.
    virtual std::vector<FlowMeshMarketStatus> flowMeshMarkets(
        const std::optional<uint256>& account_id) = 0;
    virtual std::optional<FlowMeshMarketStatus> flowMeshMarketStatus(
        const uint256& market_id,
        const std::optional<uint256>& account_id) = 0;
    //! Current active-chain registry fact (not the 30-deep runtime view).
    virtual bool flowMeshMarketEstablished(const uint256& market_id) = 0;
    virtual bool submitFlowMeshAction(const uint256& market_id,
                                      const flowmesh::Action& action,
                                      std::string& error) = 0;
    virtual bool armFlowMeshSeatKeys(const std::vector<bls::SecretKey>& keys,
                                     std::string& error) = 0;
    virtual bool disarmFlowMeshSeatKeys(std::string& error) = 0;
    virtual std::optional<FlowMeshPendingCheckpoint> nextFlowMeshCheckpoint(
        const uint256& market_id, std::string& error) = 0;
    virtual std::optional<FlowMeshVaultOperation> flowMeshVaultOperation(
        const uint256& effect_id, std::string& error) = 0;
    virtual std::vector<FlowMeshVaultOperation> flowMeshVaultOperations(
        const std::optional<uint256>& market_id, std::string& error) = 0;

    //! Get internal node context. Useful for testing, but not
    //! accessible across processes.
    virtual node::NodeContext* context() { return nullptr; }
};

//! Interface to let node manage chain clients (wallets, or maybe tools for
//! monitoring and analysis in the future).
class ChainClient
{
public:
    virtual ~ChainClient() = default;

    //! Register rpcs.
    virtual void registerRpcs() = 0;

    //! Check for errors before loading.
    virtual bool verify() = 0;

    //! Load saved state.
    virtual bool load() = 0;

    //! Start client execution and provide a scheduler.
    virtual void start(CScheduler& scheduler) = 0;

    //! Shut down client.
    virtual void stop() = 0;

    //! Set mock time.
    virtual void setMockTime(int64_t time) = 0;

    //! Mock the scheduler to fast forward in time.
    virtual void schedulerMockForward(std::chrono::seconds delta_seconds) = 0;
};

//! Return implementation of Chain interface.
std::unique_ptr<Chain> MakeChain(node::NodeContext& node);

} // namespace interfaces

#endif // BITCOIN_INTERFACES_CHAIN_H
