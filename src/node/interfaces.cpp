// Copyright (c) 2018-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addrdb.h>
#include <banman.h>
#include <blockfilter.h>
#include <chain.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <external_signer.h>
#include <index/blockfilterindex.h>
#include <init.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <interfaces/mining.h>
#include <interfaces/node.h>
#include <interfaces/types.h>
#include <interfaces/wallet.h>
#include <kernel/chain.h>
#include <kernel/context.h>
#include <kernel/mempool_entry.h>
#include <logging.h>
#include <mapport.h>
#include <net.h>
#include <net_processing.h>
#include <netaddress.h>
#include <netbase.h>
#include <node/blockstorage.h>
#include <node/coin.h>
#include <consensus/era.h>
#include <modern/chain_domain.h>
#include <modern/fn_pod.h>
#include <node/context.h>
#include <node/finality_binding_index.h>
#include <node/finality_signature.h>
#include <node/finality_tracker.h>
#include <node/flowmesh_service.h>
#include <node/flowmesh_vault_index.h>
#include <node/interface_ui.h>
#include <node/mini_miner.h>
#include <node/miner.h>
#include <node/staking.h>
#include <node/kernel_notifications.h>
#include <node/transaction.h>
#include <node/types.h>
#include <node/warnings.h>
#include <policy/feerate.h>
#include <policy/fees/block_policy_estimator.h>
#include <policy/policy.h>
#include <policy/rbf.h>
#include <policy/settings.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <rpc/blockchain.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <support/allocators/secure.h>
#include <sync.h>
#include <txmempool.h>
#include <uint256.h>
#include <univalue.h>
#include <util/check.h>
#include <util/result.h>
#include <util/signalinterrupt.h>
#include <util/string.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <any>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include <boost/signals2/signal.hpp>

using interfaces::BlockRef;
using interfaces::BlockTemplate;
using interfaces::BlockTip;
using interfaces::Chain;
using interfaces::FoundBlock;
using interfaces::Handler;
using interfaces::MakeSignalHandler;
using interfaces::Mining;
using interfaces::Node;
using interfaces::WalletLoader;
using kernel::ChainstateRole;
using node::BlockAssembler;
using node::BlockWaitOptions;
using node::CoinbaseTx;
using util::Join;

namespace node {
// All members of the classes in this namespace are intentionally public, as the
// classes themselves are private.
namespace {
#ifdef ENABLE_EXTERNAL_SIGNER
class ExternalSignerImpl : public interfaces::ExternalSigner
{
public:
    ExternalSignerImpl(::ExternalSigner signer) : m_signer(std::move(signer)) {}
    std::string getName() override { return m_signer.m_name; }
    ::ExternalSigner m_signer;
};
#endif

class NodeImpl : public Node
{
public:
    explicit NodeImpl(NodeContext& context) { setContext(&context); }
    void initLogging() override { InitLogging(args()); }
    void initParameterInteraction() override { InitParameterInteraction(args()); }
    bilingual_str getWarnings() override { return Join(Assert(m_context->warnings)->GetMessages(), Untranslated("<hr />")); }
    int getExitStatus() override { return Assert(m_context)->exit_status.load(); }
    BCLog::CategoryMask getLogCategories() override { return LogInstance().GetCategoryMask(); }
    bool baseInitialize() override
    {
        if (!AppInitBasicSetup(args(), Assert(context())->exit_status)) return false;
        if (!AppInitParameterInteraction(args())) return false;

        m_context->warnings = std::make_unique<node::Warnings>();
        m_context->kernel = std::make_unique<kernel::Context>();
        m_context->ecc_context = std::make_unique<ECC_Context>();
        if (!AppInitSanityChecks(*m_context->kernel)) return false;

        if (!AppInitLockDirectories()) return false;
        if (!AppInitInterfaces(*m_context)) return false;

        return true;
    }
    bool appInitMain(interfaces::BlockAndHeaderTipInfo* tip_info) override
    {
        if (AppInitMain(*m_context, tip_info)) return true;
        // Error during initialization, set exit status before continue
        m_context->exit_status.store(EXIT_FAILURE);
        return false;
    }
    void appShutdown() override
    {
        Shutdown(*m_context);
    }
    void startShutdown() override
    {
        NodeContext& ctx{*Assert(m_context)};
        if (!(Assert(ctx.shutdown_request))()) {
            LogError("Failed to send shutdown signal\n");
        }
        Interrupt(*m_context);
    }
    bool shutdownRequested() override { return ShutdownRequested(*Assert(m_context)); };
    bool isSettingIgnored(const std::string& name) override
    {
        bool ignored = false;
        args().LockSettings([&](common::Settings& settings) {
            if (auto* options = common::FindKey(settings.command_line_options, name)) {
                ignored = !options->empty();
            }
        });
        return ignored;
    }
    common::SettingsValue getPersistentSetting(const std::string& name) override { return args().GetPersistentSetting(name); }
    void updateRwSetting(const std::string& name, const common::SettingsValue& value) override
    {
        args().LockSettings([&](common::Settings& settings) {
            if (value.isNull()) {
                settings.rw_settings.erase(name);
            } else {
                settings.rw_settings[name] = value;
            }
        });
        args().WriteSettingsFile();
    }
    void forceSetting(const std::string& name, const common::SettingsValue& value) override
    {
        args().LockSettings([&](common::Settings& settings) {
            if (value.isNull()) {
                settings.forced_settings.erase(name);
            } else {
                settings.forced_settings[name] = value;
            }
        });
    }
    void resetSettings() override
    {
        args().WriteSettingsFile(/*errors=*/nullptr, /*backup=*/true);
        args().LockSettings([&](common::Settings& settings) {
            settings.rw_settings.clear();
        });
        args().WriteSettingsFile();
    }
    void mapPort(bool enable) override { StartMapPort(enable); }
    bool getProxy(Network net, Proxy& proxy_info) override { return GetProxy(net, proxy_info); }
    size_t getNodeCount(ConnectionDirection flags) override
    {
        return m_context->connman ? m_context->connman->GetNodeCount(flags) : 0;
    }
    bool getNodesStats(NodesStats& stats) override
    {
        stats.clear();

        if (m_context->connman) {
            std::vector<CNodeStats> stats_temp;
            m_context->connman->GetNodeStats(stats_temp);

            stats.reserve(stats_temp.size());
            for (auto& node_stats_temp : stats_temp) {
                stats.emplace_back(std::move(node_stats_temp), false, CNodeStateStats());
            }

            // Try to retrieve the CNodeStateStats for each node.
            if (m_context->peerman) {
                TRY_LOCK(::cs_main, lockMain);
                if (lockMain) {
                    for (auto& node_stats : stats) {
                        std::get<1>(node_stats) =
                            m_context->peerman->GetNodeStateStats(std::get<0>(node_stats).nodeid, std::get<2>(node_stats));
                    }
                }
            }
            return true;
        }
        return false;
    }
    bool getBanned(banmap_t& banmap) override
    {
        if (m_context->banman) {
            m_context->banman->GetBanned(banmap);
            return true;
        }
        return false;
    }
    bool ban(const CNetAddr& net_addr, int64_t ban_time_offset) override
    {
        if (m_context->banman) {
            m_context->banman->Ban(net_addr, ban_time_offset);
            return true;
        }
        return false;
    }
    bool unban(const CSubNet& ip) override
    {
        if (m_context->banman) {
            m_context->banman->Unban(ip);
            return true;
        }
        return false;
    }
    bool disconnectByAddress(const CNetAddr& net_addr) override
    {
        if (m_context->connman) {
            return m_context->connman->DisconnectNode(net_addr);
        }
        return false;
    }
    bool disconnectById(NodeId id) override
    {
        if (m_context->connman) {
            return m_context->connman->DisconnectNode(id);
        }
        return false;
    }
    std::vector<std::unique_ptr<interfaces::ExternalSigner>> listExternalSigners() override
    {
#ifdef ENABLE_EXTERNAL_SIGNER
        std::vector<ExternalSigner> signers = {};
        const std::string command = args().GetArg("-signer", "");
        if (command == "") return {};
        ExternalSigner::Enumerate(command, signers, Params().GetChainTypeString());
        std::vector<std::unique_ptr<interfaces::ExternalSigner>> result;
        result.reserve(signers.size());
        for (auto& signer : signers) {
            result.emplace_back(std::make_unique<ExternalSignerImpl>(std::move(signer)));
        }
        return result;
#else
        // This result is indistinguishable from a successful call that returns
        // no signers. For the current GUI this doesn't matter, because the wallet
        // creation dialog disables the external signer checkbox in both
        // cases. The return type could be changed to std::optional<std::vector>
        // (or something that also includes error messages) if this distinction
        // becomes important.
        return {};
#endif // ENABLE_EXTERNAL_SIGNER
    }
    int64_t getTotalBytesRecv() override { return m_context->connman ? m_context->connman->GetTotalBytesRecv() : 0; }
    int64_t getTotalBytesSent() override { return m_context->connman ? m_context->connman->GetTotalBytesSent() : 0; }
    size_t getMempoolSize() override { return m_context->mempool ? m_context->mempool->size() : 0; }
    size_t getMempoolDynamicUsage() override { return m_context->mempool ? m_context->mempool->DynamicMemoryUsage() : 0; }
    size_t getMempoolMaxUsage() override { return m_context->mempool ? m_context->mempool->m_opts.max_size_bytes : 0; }
    bool getHeaderTip(int& height, int64_t& block_time) override
    {
        LOCK(::cs_main);
        auto best_header = chainman().m_best_header;
        if (best_header) {
            height = best_header->nHeight;
            block_time = best_header->GetBlockTime();
            return true;
        }
        return false;
    }
    std::map<CNetAddr, LocalServiceInfo> getNetLocalAddresses() override
    {
        if (m_context->connman)
            return m_context->connman->getNetLocalAddresses();
        else
            return {};
    }
    int getNumBlocks() override
    {
        LOCK(::cs_main);
        return chainman().ActiveChain().Height();
    }
    uint256 getBestBlockHash() override
    {
        const CBlockIndex* tip = WITH_LOCK(::cs_main, return chainman().ActiveChain().Tip());
        return tip ? tip->GetBlockHash() : chainman().GetConsensus().hashGenesisBlock;
    }
    int64_t getLastBlockTime() override
    {
        LOCK(::cs_main);
        if (chainman().ActiveChain().Tip()) {
            return chainman().ActiveChain().Tip()->GetBlockTime();
        }
        return chainman().GetParams().GenesisBlock().GetBlockTime(); // Genesis block's time of current network
    }
    double getVerificationProgress() override
    {
        LOCK(chainman().GetMutex());
        return chainman().GuessVerificationProgress(chainman().ActiveTip());
    }
    bool isInitialBlockDownload() override
    {
        return chainman().IsInitialBlockDownload();
    }
    bool isLoadingBlocks() override { return chainman().m_blockman.LoadingBlocks(); }
    void setNetworkActive(bool active) override
    {
        if (m_context->connman) {
            m_context->connman->SetNetworkActive(active);
        }
    }
    bool getNetworkActive() override { return m_context->connman && m_context->connman->GetNetworkActive(); }
    CFeeRate getDustRelayFee() override
    {
        if (!m_context->mempool) return CFeeRate{DUST_RELAY_TX_FEE};
        return m_context->mempool->m_opts.dust_relay_feerate;
    }
    UniValue executeRpc(const std::string& command, const UniValue& params, const std::string& uri) override
    {
        JSONRPCRequest req;
        req.context = m_context;
        req.params = params;
        req.strMethod = command;
        req.URI = uri;
        return ::tableRPC.execute(req);
    }
    std::vector<std::string> listRpcCommands() override { return ::tableRPC.listCommands(); }
    std::optional<Coin> getUnspentOutput(const COutPoint& output) override
    {
        LOCK(::cs_main);
        return chainman().ActiveChainstate().CoinsTip().GetCoin(output);
    }
    TransactionError broadcastTransaction(CTransactionRef tx, CAmount max_tx_fee, std::string& err_string) override
    {
        return BroadcastTransaction(*m_context,
                                    std::move(tx),
                                    err_string,
                                    max_tx_fee,
                                    TxBroadcast::MEMPOOL_AND_BROADCAST_TO_ALL,
                                    /*wait_callback=*/false);
    }
    WalletLoader& walletLoader() override
    {
        return *Assert(m_context->wallet_loader);
    }
    std::unique_ptr<Handler> handleInitMessage(InitMessageFn fn) override
    {
        return MakeSignalHandler(::uiInterface.InitMessage_connect(fn));
    }
    std::unique_ptr<Handler> handleMessageBox(MessageBoxFn fn) override
    {
        return MakeSignalHandler(::uiInterface.ThreadSafeMessageBox_connect(fn));
    }
    std::unique_ptr<Handler> handleQuestion(QuestionFn fn) override
    {
        return MakeSignalHandler(::uiInterface.ThreadSafeQuestion_connect(fn));
    }
    std::unique_ptr<Handler> handleShowProgress(ShowProgressFn fn) override
    {
        return MakeSignalHandler(::uiInterface.ShowProgress_connect(fn));
    }
    std::unique_ptr<Handler> handleInitWallet(InitWalletFn fn) override
    {
        return MakeSignalHandler(::uiInterface.InitWallet_connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyNumConnectionsChanged(NotifyNumConnectionsChangedFn fn) override
    {
        return MakeSignalHandler(::uiInterface.NotifyNumConnectionsChanged_connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyNetworkActiveChanged(NotifyNetworkActiveChangedFn fn) override
    {
        return MakeSignalHandler(::uiInterface.NotifyNetworkActiveChanged_connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyAlertChanged(NotifyAlertChangedFn fn) override
    {
        return MakeSignalHandler(::uiInterface.NotifyAlertChanged_connect(fn));
    }
    std::unique_ptr<Handler> handleBannedListChanged(BannedListChangedFn fn) override
    {
        return MakeSignalHandler(::uiInterface.BannedListChanged_connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyBlockTip(NotifyBlockTipFn fn) override
    {
        return MakeSignalHandler(::uiInterface.NotifyBlockTip_connect([fn](SynchronizationState sync_state, const CBlockIndex& block, double verification_progress) {
            fn(sync_state, BlockTip{block.nHeight, block.GetBlockTime(), block.GetBlockHash()}, verification_progress);
        }));
    }
    std::unique_ptr<Handler> handleNotifyHeaderTip(NotifyHeaderTipFn fn) override
    {
        return MakeSignalHandler(
            ::uiInterface.NotifyHeaderTip_connect([fn](SynchronizationState sync_state, int64_t height, int64_t timestamp, bool presync) {
                fn(sync_state, BlockTip{(int)height, timestamp, uint256{}}, presync);
            }));
    }
    NodeContext* context() override { return m_context; }
    void setContext(NodeContext* context) override
    {
        m_context = context;
    }
    ArgsManager& args() { return *Assert(Assert(m_context)->args); }
    ChainstateManager& chainman() { return *Assert(m_context->chainman); }
    NodeContext* m_context{nullptr};
};

// NOLINTNEXTLINE(misc-no-recursion)
bool FillBlock(const CBlockIndex* index, const FoundBlock& block, UniqueLock<RecursiveMutex>& lock, const CChain& active, const BlockManager& blockman) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    if (!index) return false;
    if (block.m_hash) *block.m_hash = index->GetBlockHash();
    if (block.m_height) *block.m_height = index->nHeight;
    if (block.m_time) *block.m_time = index->GetBlockTime();
    if (block.m_max_time) *block.m_max_time = index->GetBlockTimeMax();
    if (block.m_mtp_time) *block.m_mtp_time = index->GetMedianTimePast();
    if (block.m_in_active_chain) *block.m_in_active_chain = active[index->nHeight] == index;
    if (block.m_locator) { *block.m_locator = GetLocator(index); }
    if (block.m_next_block) FillBlock(active[index->nHeight] == index ? active[index->nHeight + 1] : nullptr, *block.m_next_block, lock, active, blockman);
    if (block.m_data) {
        REVERSE_LOCK(lock, cs_main);
        if (!blockman.ReadBlock(*block.m_data, *index)) block.m_data->SetNull();
    }
    block.found = true;
    return true;
}

class NotificationsProxy : public CValidationInterface
{
public:
    explicit NotificationsProxy(std::shared_ptr<Chain::Notifications> notifications)
        : m_notifications(std::move(notifications)) {}
    virtual ~NotificationsProxy() = default;
    void TransactionAddedToMempool(const NewMempoolTransactionInfo& tx, uint64_t mempool_sequence) override
    {
        m_notifications->transactionAddedToMempool(tx.info.m_tx);
    }
    void TransactionRemovedFromMempool(const CTransactionRef& tx, MemPoolRemovalReason reason, uint64_t mempool_sequence) override
    {
        m_notifications->transactionRemovedFromMempool(tx, reason);
    }
    void BlockConnected(const ChainstateRole& role, const std::shared_ptr<const CBlock>& block, const CBlockIndex* index) override
    {
        m_notifications->blockConnected(role, kernel::MakeBlockInfo(index, block.get()));
    }
    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* index) override
    {
        m_notifications->blockDisconnected(kernel::MakeBlockInfo(index, block.get()));
    }
    void UpdatedBlockTip(const CBlockIndex* index, const CBlockIndex* fork_index, bool is_ibd) override
    {
        m_notifications->updatedBlockTip();
    }
    void ChainStateFlushed(const ChainstateRole& role, const CBlockLocator& locator) override
    {
        m_notifications->chainStateFlushed(role, locator);
    }
    std::shared_ptr<Chain::Notifications> m_notifications;
};

class NotificationsHandlerImpl : public Handler
{
public:
    explicit NotificationsHandlerImpl(ValidationSignals& signals, std::shared_ptr<Chain::Notifications> notifications)
        : m_signals{signals}, m_proxy{std::make_shared<NotificationsProxy>(std::move(notifications))}
    {
        m_signals.RegisterSharedValidationInterface(m_proxy);
    }
    ~NotificationsHandlerImpl() override { disconnect(); }
    void disconnect() override
    {
        if (m_proxy) {
            m_signals.UnregisterSharedValidationInterface(m_proxy);
            m_proxy.reset();
        }
    }
    ValidationSignals& m_signals;
    std::shared_ptr<NotificationsProxy> m_proxy;
};

class RpcHandlerImpl : public Handler
{
public:
    explicit RpcHandlerImpl(const CRPCCommand& command) : m_command(command), m_wrapped_command(&command)
    {
        m_command.actor = [this](const JSONRPCRequest& request, UniValue& result, bool last_handler) {
            if (!m_wrapped_command) return false;
            try {
                return m_wrapped_command->actor(request, result, last_handler);
            } catch (const UniValue& e) {
                // If this is not the last handler and a wallet not found
                // exception was thrown, return false so the next handler can
                // try to handle the request. Otherwise, reraise the exception.
                if (!last_handler) {
                    const UniValue& code = e["code"];
                    if (code.isNum() && code.getInt<int>() == RPC_WALLET_NOT_FOUND) {
                        return false;
                    }
                }
                throw;
            }
        };
        ::tableRPC.appendCommand(m_command.name, &m_command);
    }

    void disconnect() final
    {
        if (m_wrapped_command) {
            m_wrapped_command = nullptr;
            ::tableRPC.removeCommand(m_command.name, &m_command);
        }
    }

    ~RpcHandlerImpl() override { disconnect(); }

    CRPCCommand m_command;
    const CRPCCommand* m_wrapped_command;
};

class ChainImpl : public Chain
{
public:
    explicit ChainImpl(NodeContext& node) : m_node(node) {}
    std::optional<int> getHeight() override
    {
        const int height{WITH_LOCK(::cs_main, return chainman().ActiveChain().Height())};
        return height >= 0 ? std::optional{height} : std::nullopt;
    }
    uint256 getBlockHash(int height) override
    {
        LOCK(::cs_main);
        return Assert(chainman().ActiveChain()[height])->GetBlockHash();
    }
    bool haveBlockOnDisk(int height) override
    {
        LOCK(::cs_main);
        const CBlockIndex* block{chainman().ActiveChain()[height]};
        return block && ((block->nStatus & BLOCK_HAVE_DATA) != 0) && block->nTx > 0;
    }
    std::optional<int> findLocatorFork(const CBlockLocator& locator) override
    {
        LOCK(::cs_main);
        if (const CBlockIndex* fork = chainman().ActiveChainstate().FindForkInGlobalIndex(locator)) {
            return fork->nHeight;
        }
        return std::nullopt;
    }
    bool hasBlockFilterIndex(BlockFilterType filter_type) override
    {
        return GetBlockFilterIndex(filter_type) != nullptr;
    }
    std::optional<bool> blockFilterMatchesAny(BlockFilterType filter_type, const uint256& block_hash, const GCSFilter::ElementSet& filter_set) override
    {
        const BlockFilterIndex* block_filter_index{GetBlockFilterIndex(filter_type)};
        if (!block_filter_index) return std::nullopt;

        BlockFilter filter;
        const CBlockIndex* index{WITH_LOCK(::cs_main, return chainman().m_blockman.LookupBlockIndex(block_hash))};
        if (index == nullptr || !block_filter_index->LookupFilter(index, filter)) return std::nullopt;
        return filter.GetFilter().MatchAny(filter_set);
    }
    bool findBlock(const uint256& hash, const FoundBlock& block) override
    {
        WAIT_LOCK(cs_main, lock);
        return FillBlock(chainman().m_blockman.LookupBlockIndex(hash), block, lock, chainman().ActiveChain(), chainman().m_blockman);
    }
    bool findFirstBlockWithTimeAndHeight(int64_t min_time, int min_height, const FoundBlock& block) override
    {
        WAIT_LOCK(cs_main, lock);
        const CChain& active = chainman().ActiveChain();
        return FillBlock(active.FindEarliestAtLeast(min_time, min_height), block, lock, active, chainman().m_blockman);
    }
    bool findAncestorByHeight(const uint256& block_hash, int ancestor_height, const FoundBlock& ancestor_out) override
    {
        WAIT_LOCK(cs_main, lock);
        const CChain& active = chainman().ActiveChain();
        if (const CBlockIndex* block = chainman().m_blockman.LookupBlockIndex(block_hash)) {
            if (const CBlockIndex* ancestor = block->GetAncestor(ancestor_height)) {
                return FillBlock(ancestor, ancestor_out, lock, active, chainman().m_blockman);
            }
        }
        return FillBlock(nullptr, ancestor_out, lock, active, chainman().m_blockman);
    }
    bool findAncestorByHash(const uint256& block_hash, const uint256& ancestor_hash, const FoundBlock& ancestor_out) override
    {
        WAIT_LOCK(cs_main, lock);
        const CBlockIndex* block = chainman().m_blockman.LookupBlockIndex(block_hash);
        const CBlockIndex* ancestor = chainman().m_blockman.LookupBlockIndex(ancestor_hash);
        if (block && ancestor && block->GetAncestor(ancestor->nHeight) != ancestor) ancestor = nullptr;
        return FillBlock(ancestor, ancestor_out, lock, chainman().ActiveChain(), chainman().m_blockman);
    }
    bool findCommonAncestor(const uint256& block_hash1, const uint256& block_hash2, const FoundBlock& ancestor_out, const FoundBlock& block1_out, const FoundBlock& block2_out) override
    {
        WAIT_LOCK(cs_main, lock);
        const CChain& active = chainman().ActiveChain();
        const CBlockIndex* block1 = chainman().m_blockman.LookupBlockIndex(block_hash1);
        const CBlockIndex* block2 = chainman().m_blockman.LookupBlockIndex(block_hash2);
        const CBlockIndex* ancestor = block1 && block2 ? LastCommonAncestor(block1, block2) : nullptr;
        // Using & instead of && below to avoid short circuiting and leaving
        // output uninitialized. Cast bool to int to avoid -Wbitwise-instead-of-logical
        // compiler warnings.
        return int{FillBlock(ancestor, ancestor_out, lock, active, chainman().m_blockman)} &
               int{FillBlock(block1, block1_out, lock, active, chainman().m_blockman)} &
               int{FillBlock(block2, block2_out, lock, active, chainman().m_blockman)};
    }
    void findCoins(std::map<COutPoint, Coin>& coins) override { return FindCoins(m_node, coins); }
    double guessVerificationProgress(const uint256& block_hash) override
    {
        LOCK(chainman().GetMutex());
        return chainman().GuessVerificationProgress(chainman().m_blockman.LookupBlockIndex(block_hash));
    }
    bool hasBlocks(const uint256& block_hash, int min_height, std::optional<int> max_height) override
    {
        // hasBlocks returns true if all ancestors of block_hash in specified
        // range have block data (are not pruned), false if any ancestors in
        // specified range are missing data.
        //
        // For simplicity and robustness, min_height and max_height are only
        // used to limit the range, and passing min_height that's too low or
        // max_height that's too high will not crash or change the result.
        LOCK(::cs_main);
        if (const CBlockIndex* block = chainman().m_blockman.LookupBlockIndex(block_hash)) {
            if (max_height && block->nHeight >= *max_height) block = block->GetAncestor(*max_height);
            for (; block->nStatus & BLOCK_HAVE_DATA; block = block->pprev) {
                // Check pprev to not segfault if min_height is too low
                if (block->nHeight <= min_height || !block->pprev) return true;
            }
        }
        return false;
    }
    RBFTransactionState isRBFOptIn(const CTransaction& tx) override
    {
        if (!m_node.mempool) return IsRBFOptInEmptyMempool(tx);
        LOCK(m_node.mempool->cs);
        return IsRBFOptIn(tx, *m_node.mempool);
    }
    bool isInMempool(const Txid& txid) override
    {
        if (!m_node.mempool) return false;
        return m_node.mempool->exists(txid);
    }
    bool hasDescendantsInMempool(const Txid& txid) override
    {
        if (!m_node.mempool) return false;
        return m_node.mempool->HasDescendants(txid);
    }
    bool broadcastTransaction(const CTransactionRef& tx,
        const CAmount& max_tx_fee,
        TxBroadcast broadcast_method,
        std::string& err_string) override
    {
        const TransactionError err = BroadcastTransaction(m_node, tx, err_string, max_tx_fee, broadcast_method, /*wait_callback=*/false);
        // Chain clients only care about failures to accept the tx to the mempool. Disregard non-mempool related failures.
        // Note: this will need to be updated if BroadcastTransactions() is updated to return other non-mempool failures
        // that Chain clients do not need to know about.
        return TransactionError::OK == err;
    }
    void getTransactionAncestry(const Txid& txid, size_t& ancestors, size_t& cluster_count, size_t* ancestorsize, CAmount* ancestorfees) override
    {
        ancestors = cluster_count = 0;
        if (!m_node.mempool) return;
        m_node.mempool->GetTransactionAncestry(txid, ancestors, cluster_count, ancestorsize, ancestorfees);
    }

    std::map<COutPoint, CAmount> calculateIndividualBumpFees(const std::vector<COutPoint>& outpoints, const CFeeRate& target_feerate) override
    {
        if (!m_node.mempool) {
            std::map<COutPoint, CAmount> bump_fees;
            for (const auto& outpoint : outpoints) {
                bump_fees.emplace(outpoint, 0);
            }
            return bump_fees;
        }
        return MiniMiner(*m_node.mempool, outpoints).CalculateBumpFees(target_feerate);
    }

    std::optional<CAmount> calculateCombinedBumpFee(const std::vector<COutPoint>& outpoints, const CFeeRate& target_feerate) override
    {
        if (!m_node.mempool) {
            return 0;
        }
        return MiniMiner(*m_node.mempool, outpoints).CalculateTotalBumpFees(target_feerate);
    }
    void getPackageLimits(unsigned int& limit_ancestor_count, unsigned int& limit_descendant_count) override
    {
        const CTxMemPool::Limits default_limits{};

        const CTxMemPool::Limits& limits{m_node.mempool ? m_node.mempool->m_opts.limits : default_limits};

        limit_ancestor_count = limits.ancestor_count;
        limit_descendant_count = limits.descendant_count;
    }
    util::Result<void> checkChainLimits(const CTransactionRef& tx) override
    {
        if (!m_node.mempool) return {};
        if (!m_node.mempool->CheckPolicyLimits(tx)) {
            return util::Error{Untranslated("too many unconfirmed transactions in cluster")};
        }
        return {};
    }
    CFeeRate estimateSmartFee(int num_blocks, bool conservative, FeeCalculation* calc) override
    {
        if (!m_node.fee_estimator) return {};
        return m_node.fee_estimator->estimateSmartFee(num_blocks, calc, conservative);
    }
    unsigned int estimateMaxBlocks() override
    {
        if (!m_node.fee_estimator) return 0;
        return m_node.fee_estimator->HighestTargetTracked(FeeEstimateHorizon::LONG_HALFLIFE);
    }
    CFeeRate mempoolMinFee() override
    {
        if (!m_node.mempool) return {};
        return m_node.mempool->GetMinFee();
    }
    CFeeRate relayMinFee() override
    {
        if (!m_node.mempool) return CFeeRate{DEFAULT_MIN_RELAY_TX_FEE};
        return m_node.mempool->m_opts.min_relay_feerate;
    }
    CFeeRate relayIncrementalFee() override
    {
        if (!m_node.mempool) return CFeeRate{DEFAULT_INCREMENTAL_RELAY_FEE};
        return m_node.mempool->m_opts.incremental_relay_feerate;
    }
    CFeeRate relayDustFee() override
    {
        if (!m_node.mempool) return CFeeRate{DUST_RELAY_TX_FEE};
        return m_node.mempool->m_opts.dust_relay_feerate;
    }
    bool havePruned() override
    {
        LOCK(::cs_main);
        return chainman().m_blockman.m_have_pruned;
    }
    std::optional<int> getPruneHeight() override
    {
        LOCK(chainman().GetMutex());
        return GetPruneHeight(chainman().m_blockman, chainman().ActiveChain());
    }
    bool isReadyToBroadcast() override { return !chainman().m_blockman.LoadingBlocks() && !isInitialBlockDownload(); }
    bool isInitialBlockDownload() override
    {
        return chainman().IsInitialBlockDownload();
    }
    bool shutdownRequested() override { return ShutdownRequested(m_node); }
    void initMessage(const std::string& message) override { ::uiInterface.InitMessage(message); }
    void initWarning(const bilingual_str& message) override { InitWarning(message); }
    void initError(const bilingual_str& message) override { InitError(message); }
    void showProgress(const std::string& title, int progress, bool resume_possible) override
    {
        ::uiInterface.ShowProgress(title, progress, resume_possible);
    }
    std::unique_ptr<Handler> handleNotifications(std::shared_ptr<Notifications> notifications) override
    {
        return std::make_unique<NotificationsHandlerImpl>(validation_signals(), std::move(notifications));
    }
    void waitForNotificationsIfTipChanged(const uint256& old_tip) override
    {
        if (!old_tip.IsNull() && old_tip == WITH_LOCK(::cs_main, return chainman().ActiveChain().Tip()->GetBlockHash())) return;
        validation_signals().SyncWithValidationInterfaceQueue();
    }
    void waitForNotifications() override
    {
        validation_signals().SyncWithValidationInterfaceQueue();
    }
    std::unique_ptr<Handler> handleRpc(const CRPCCommand& command) override
    {
        return std::make_unique<RpcHandlerImpl>(command);
    }
    bool rpcEnableDeprecated(const std::string& method) override { return IsDeprecatedRPCEnabled(method); }
    common::SettingsValue getSetting(const std::string& name) override
    {
        return args().GetSetting(name);
    }
    std::vector<common::SettingsValue> getSettingsList(const std::string& name) override
    {
        return args().GetSettingsList(name);
    }
    common::SettingsValue getRwSetting(const std::string& name) override
    {
        common::SettingsValue result;
        args().LockSettings([&](const common::Settings& settings) {
            if (const common::SettingsValue* value = common::FindKey(settings.rw_settings, name)) {
                result = *value;
            }
        });
        return result;
    }
    bool updateRwSetting(const std::string& name,
                         const interfaces::SettingsUpdate& update_settings_func) override
    {
        std::optional<interfaces::SettingsAction> action;
        args().LockSettings([&](common::Settings& settings) {
            if (auto* value = common::FindKey(settings.rw_settings, name)) {
                action = update_settings_func(*value);
                if (value->isNull()) settings.rw_settings.erase(name);
            } else {
                UniValue new_value;
                action = update_settings_func(new_value);
                if (!new_value.isNull()) settings.rw_settings[name] = std::move(new_value);
            }
        });
        if (!action) return false;
        // Now dump value to disk if requested
        return *action != interfaces::SettingsAction::WRITE || args().WriteSettingsFile();
    }
    bool overwriteRwSetting(const std::string& name, common::SettingsValue value, interfaces::SettingsAction action) override
    {
        return updateRwSetting(name, [&](common::SettingsValue& settings) {
            settings = std::move(value);
            return action;
        });
    }
    bool deleteRwSettings(const std::string& name, interfaces::SettingsAction action) override
    {
        return overwriteRwSetting(name, {}, action);
    }
    void requestMempoolTransactions(Notifications& notifications) override
    {
        if (!m_node.mempool) return;
        LOCK2(::cs_main, m_node.mempool->cs);
        for (const CTxMemPoolEntry& entry : m_node.mempool->entryAll()) {
            notifications.transactionAddedToMempool(entry.GetSharedTx());
        }
    }
    std::optional<interfaces::ModernCreationSnapshot> modernCreationSnapshot(
        const bool inspect_fn_pool, std::string& error) override
    {
        if (!m_node.mempool) {
            error = "Mempool disabled or instance not found";
            return std::nullopt;
        }
        LOCK2(::cs_main, m_node.mempool->cs);
        const CBlockIndex* tip{chainman().ActiveChain().Tip()};
        if (!tip) {
            error = "Active chain has no tip";
            return std::nullopt;
        }
        interfaces::ModernCreationSnapshot snapshot;
        snapshot.tip_hash = tip->GetBlockHash();
        snapshot.next_height = tip->nHeight + 1;
        if (!inspect_fn_pool) {
            error.clear();
            return snapshot;
        }

        const Consensus::Params& params{chainman().GetConsensus()};
        if (!params.fn_pod_activation_height ||
            tip->nHeight < *params.fn_pod_activation_height) {
            snapshot.fn_issued_before = uint32_t{0};
        } else if (tip->m_fn_pod_issued_total_known) {
            snapshot.fn_issued_before = tip->m_fn_pod_issued_total;
        }
        for (const CTxMemPoolEntry& entry : m_node.mempool->entryAll()) {
            if (modern::HasModernFnPodDeclaration(entry.GetTx())) {
                snapshot.pending_fn_pod = true;
                break;
            }
        }
        error.clear();
        return snapshot;
    }
    bool hasAssumedValidChain() override
    {
        LOCK(::cs_main);
        return bool{chainman().CurrentChainstate().m_from_snapshot_blockhash};
    }
    bool startStaking(const CKey& validator_key, const CScript& coinbase_script, std::string& error) override
    {
        if (!m_node.staking) {
            error = "staking is not available in this node";
            return false;
        }
        return m_node.staking->Start(validator_key, coinbase_script, error);
    }
    void stopStaking() override
    {
        if (m_node.staking) m_node.staking->Stop();
    }
    interfaces::StakingStatus stakingStatus(const std::optional<std::array<unsigned char, 32>>& validator_key) override
    {
        if (!m_node.staking) {
            interfaces::StakingStatus status;
            status.state = "unavailable";
            return status;
        }
        return m_node.staking->Status(validator_key);
    }
    interfaces::FinalityStatus finalityStatus(const std::optional<std::array<unsigned char, 32>>& validator_key) override
    {
        interfaces::FinalityStatus out;
        const Consensus::Params& consensus{chainman().GetConsensus()};
        out.configured = consensus.legacy_b3coin && consensus.modern_pos.has_value() &&
                         Consensus::LegacyBoundaryPinned(consensus);
        if (!out.configured) return out;
        if (const auto domain{modern::ModernChainDomain(consensus.hashGenesisBlock, *consensus.legacy_final_hash)}) {
            out.chain_domain = *domain;
        }
        LOCK(::cs_main);
        Chainstate& chainstate{chainman().ActiveChainstate()};
        const CBlockIndex* tip{chainstate.m_chain.Tip()};
        if (const auto pin{chainstate.m_blockman.FinalityAnchor()}) {
            out.pin_height = pin->first;
            out.pin_hash = pin->second;
        }
        out.pool_checkpoints = chainstate.FinalitySignatures().TrackedCheckpoints();
        if (!tip) return out;
        // Per-validator binding (independent of the epoch state).
        if (validator_key) {
            node::FinalityBindingTracker& bindings{chainstate.ModernFinalityBindings()};
            if (bindings.Sync(chainstate.m_chain, chainstate.m_blockman, consensus, *tip)) {
                if (const auto rec{bindings.Index().Get(*validator_key)}) {
                    out.bound = true;
                    out.revoked = rec->IsRevoked();
                    out.binding_seq = rec->seq;
                    out.binding_bls_pubkey.assign(rec->bls_pubkey.begin(), rec->bls_pubkey.end());
                    out.binding_height = rec->height;
                }
            }
        }
        node::FinalityTracker& tracker{chainstate.ModernFinality()};
        if (!tracker.Sync(chainstate.m_chain, chainstate.m_blockman, consensus, *tip)) return out;
        out.active = true;
        const node::FinalityTracker::State& state{tracker.Current()};
        out.bootstrapped = state.bootstrapped;
        out.epoch = state.epoch;
        out.epoch_start = state.epoch_starts.empty() ? -1 : state.epoch_starts.back();
        out.handover_certified = state.handover_certified;
        out.lineage_broken = state.lineage_broken;
        if (state.current) {
            out.set_size = static_cast<int>(state.current->Size());
            out.total_weight = state.current->TotalWeight();
            out.quorum_weight = state.current->QuorumWeight();
            out.current_set_hash = state.current->SetHash();
            if (validator_key) {
                if (const auto index{state.current->IndexOf(*validator_key)}) {
                    out.in_current_set = true;
                    out.member_weight = state.current->Members()[*index].weight;
                }
            }
        }
        if (state.next) out.next_set_hash = state.next->SetHash();
        if (state.finalized) {
            out.finalized_height = state.finalized->height;
            out.finalized_hash = state.finalized->block_hash;
            out.finalized_epoch = state.finalized->epoch;
        }
        return out;
    }
    bool armFinalitySigner(const bls::SecretKey& key, const std::array<unsigned char, 32>& validator_key,
                           std::string& error) override
    {
        if (!m_node.staking) {
            error = "staking is not available in this node";
            return false;
        }
        return m_node.staking->SetFinalityKey(key, validator_key, error);
    }
    bool disarmFinalitySigner(std::string& error) override
    {
        if (!m_node.staking) {
            error = "staking is not available in this node";
            return false;
        }
        return m_node.staking->ClearFinalityKey(error);
    }
    std::vector<interfaces::FlowMeshMarketStatus> flowMeshMarkets(
        const std::optional<uint256>& account_id) override
    {
        if (!m_node.flowmesh) return {};
        std::vector<interfaces::FlowMeshMarketStatus> out;
        const auto markets{m_node.flowmesh->Markets()};
        out.reserve(markets.size());
        for (const auto& market : markets) {
            const auto status{flowMeshMarketStatus(market.market_id,
                                                   account_id)};
            if (status) out.push_back(*status);
        }
        return out;
    }
    std::optional<interfaces::FlowMeshMarketStatus> flowMeshMarketStatus(
        const uint256& market_id,
        const std::optional<uint256>& account_id) override
    {
        if (!m_node.flowmesh) return std::nullopt;
        const auto market{m_node.flowmesh->Market(market_id)};
        if (!market) return std::nullopt;
        interfaces::FlowMeshMarketStatus out;
        out.available = m_node.flowmesh->Enabled();
        out.running = m_node.flowmesh->Running();
        out.domain = market->domain;
        out.market_id = market->market_id;
        out.vault_id = market->vault_id;
        out.base_asset = market->base_asset;
        out.quote_asset = market->quote_asset;
        out.execution_config_id = market->execution_config_id;
        if (const auto runtime{m_node.flowmesh->MarketStatus(market_id)}) {
            out.epoch = runtime->epoch;
            out.next_microblock_sequence = runtime->next_sequence;
            out.next_effect_index = runtime->next_effect_index;
            out.round = runtime->round;
            out.last_microblock_hash = runtime->last_microblock_hash;
            out.state_root = runtime->state_root;
            out.pending_actions = runtime->pending_actions;
            out.observer_only = runtime->observer_only;
            out.paused = runtime->paused;
            out.pending_handoff = runtime->pending_handoff;
            out.halt = node::FlowMeshRuntimeHaltName(runtime->halt);
            out.error = runtime->error;
        }
        std::string checkpoint_error;
        if (const auto checkpoint{m_node.flowmesh->NextCheckpointMpa(
                market_id, checkpoint_error)}) {
            out.checkpoint_pending = true;
            out.pending_checkpoint_id = checkpoint->checkpoint_id;
            out.pending_checkpoint_sequence = checkpoint->sequence;
            out.pending_checkpoint_effect_count = checkpoint->effect_count;
        }
        if (account_id) {
            out.account_id = *account_id;
            if (const auto state{m_node.flowmesh->StateSnapshot(market_id)}) {
                out.next_account_sequence = state->NextSequence(*account_id);
                out.slot = state->Slot();
                out.base_available = state->LedgerView().Available(
                    *account_id, market->base_asset);
                out.base_reserved = state->LedgerView().Reserved(
                    *account_id, market->base_asset);
                out.b3_available = state->LedgerView().Available(
                    *account_id, modern::NativeAsset());
                out.b3_reserved = state->LedgerView().Reserved(
                    *account_id, modern::NativeAsset());
            }
        }
        return out;
    }
    bool flowMeshMarketEstablished(const uint256& market_id) override
    {
        LOCK(::cs_main);
        Chainstate& chainstate{chainman().ActiveChainstate()};
        const CBlockIndex* tip{chainstate.m_chain.Tip()};
        if (!tip) return false;
        node::FlowMeshVaultTracker& tracker{chainstate.ModernFlowMeshVaults()};
        if (!tracker.Sync(chainstate.m_chain, chainstate.m_blockman,
                          chainman().GetConsensus(), *tip)) {
            return false;
        }
        return tracker.Index().Market(market_id).has_value();
    }
    bool submitFlowMeshAction(const uint256& market_id,
                              const flowmesh::Action& action,
                              std::string& error) override
    {
        if (!m_node.flowmesh) {
            error = "FlowMesh service is not available in this node";
            return false;
        }
        return m_node.flowmesh->SubmitLocalAction(market_id, action, error);
    }
    bool armFlowMeshSeatKeys(const std::vector<bls::SecretKey>& keys,
                             std::string& error) override
    {
        if (!m_node.flowmesh) {
            error = "FlowMesh service is not available in this node";
            return false;
        }
        return m_node.flowmesh->ArmSeatKeys(keys, error);
    }
    bool disarmFlowMeshSeatKeys(std::string& error) override
    {
        if (!m_node.flowmesh) {
            error = "FlowMesh service is not available in this node";
            return false;
        }
        m_node.flowmesh->DisarmSeatKeys();
        return true;
    }
    std::optional<interfaces::FlowMeshPendingCheckpoint>
    nextFlowMeshCheckpoint(const uint256& market_id,
                           std::string& error) override
    {
        if (!m_node.flowmesh) {
            error = "FlowMesh service is not available in this node";
            return std::nullopt;
        }
        const auto pending{m_node.flowmesh->NextCheckpointMpa(market_id,
                                                              error)};
        if (!pending) return std::nullopt;
        return interfaces::FlowMeshPendingCheckpoint{
            pending->record, pending->checkpoint_id, pending->sequence,
            pending->effect_count};
    }
    std::optional<interfaces::FlowMeshVaultOperation>
    flowMeshVaultOperation(const uint256& effect_id,
                           std::string& error) override
    {
        if (!m_node.flowmesh) {
            error = "FlowMesh service is not available in this node";
            return std::nullopt;
        }
        const auto operation{m_node.flowmesh->VaultOperation(effect_id,
                                                              error)};
        if (!operation) return std::nullopt;
        interfaces::FlowMeshVaultOperation out;
        out.market_id = operation->market_id;
        out.checkpoint_id = operation->checkpoint_id;
        out.record = operation->record;
        out.effect = operation->effect;
        out.inputs.reserve(operation->inputs.size());
        for (const auto& input : operation->inputs) {
            out.inputs.push_back(
                interfaces::FlowMeshVaultInput{input.record.outpoint,
                                               input.txout});
        }
        return out;
    }
    std::vector<interfaces::FlowMeshVaultOperation>
    flowMeshVaultOperations(const std::optional<uint256>& market_id,
                            std::string& error) override
    {
        if (!m_node.flowmesh) {
            error = "FlowMesh service is not available in this node";
            return {};
        }
        const auto operations{
            m_node.flowmesh->VaultOperations(market_id, error)};
        std::vector<interfaces::FlowMeshVaultOperation> out;
        out.reserve(operations.size());
        for (const auto& operation : operations) {
            interfaces::FlowMeshVaultOperation item;
            item.market_id = operation.market_id;
            item.checkpoint_id = operation.checkpoint_id;
            item.record = operation.record;
            item.effect = operation.effect;
            item.inputs.reserve(operation.inputs.size());
            for (const auto& input : operation.inputs) {
                item.inputs.push_back(
                    interfaces::FlowMeshVaultInput{input.record.outpoint,
                                                   input.txout});
            }
            out.push_back(std::move(item));
        }
        return out;
    }

    NodeContext* context() override { return &m_node; }
    ArgsManager& args() { return *Assert(m_node.args); }
    ChainstateManager& chainman() { return *Assert(m_node.chainman); }
    ValidationSignals& validation_signals() { return *Assert(m_node.validation_signals); }
    NodeContext& m_node;
};

class BlockTemplateImpl : public BlockTemplate
{
public:
    explicit BlockTemplateImpl(BlockAssembler::Options assemble_options,
                               std::unique_ptr<CBlockTemplate> block_template,
                               NodeContext& node) : m_assemble_options(std::move(assemble_options)),
                                                    m_block_template(std::move(block_template)),
                                                    m_node(node)
    {
        assert(m_block_template);
    }

    CBlockHeader getBlockHeader() override
    {
        return m_block_template->block;
    }

    CBlock getBlock() override
    {
        return m_block_template->block;
    }

    std::vector<CAmount> getTxFees() override
    {
        return m_block_template->vTxFees;
    }

    std::vector<int64_t> getTxSigops() override
    {
        return m_block_template->vTxSigOpsCost;
    }

    CoinbaseTx getCoinbaseTx() override
    {
        return m_block_template->m_coinbase_tx;
    }

    std::vector<uint256> getCoinbaseMerklePath() override
    {
        return TransactionMerklePath(m_block_template->block, 0);
    }

    bool submitSolution(uint32_t version, uint32_t timestamp, uint32_t nonce, CTransactionRef coinbase) override
    {
        AddMerkleRootAndCoinbase(m_block_template->block, std::move(coinbase), version, timestamp, nonce);
        return chainman().ProcessNewBlock(std::make_shared<const CBlock>(m_block_template->block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/nullptr);
    }

    std::unique_ptr<BlockTemplate> waitNext(BlockWaitOptions options) override
    {
        auto new_template = WaitAndCreateNewBlock(chainman(), notifications(), m_node.mempool.get(), m_block_template, options, m_assemble_options, m_interrupt_wait);
        if (new_template) return std::make_unique<BlockTemplateImpl>(m_assemble_options, std::move(new_template), m_node);
        return nullptr;
    }

    void interruptWait() override
    {
        InterruptWait(notifications(), m_interrupt_wait);
    }

    const BlockAssembler::Options m_assemble_options;

    const std::unique_ptr<CBlockTemplate> m_block_template;

    bool m_interrupt_wait{false};
    ChainstateManager& chainman() { return *Assert(m_node.chainman); }
    KernelNotifications& notifications() { return *Assert(m_node.notifications); }
    NodeContext& m_node;
};

class MinerImpl : public Mining
{
public:
    explicit MinerImpl(NodeContext& node) : m_node(node) {}

    bool isTestChain() override
    {
        return chainman().GetParams().IsTestChain();
    }

    bool isInitialBlockDownload() override
    {
        return chainman().IsInitialBlockDownload();
    }

    std::optional<BlockRef> getTip() override
    {
        return GetTip(chainman());
    }

    std::optional<BlockRef> waitTipChanged(uint256 current_tip, MillisecondsDouble timeout) override
    {
        return WaitTipChanged(chainman(), notifications(), current_tip, timeout, m_interrupt_mining);
    }

    std::unique_ptr<BlockTemplate> createNewBlock(const BlockCreateOptions& options, bool cooldown) override
    {
        // Reject too-small values instead of clamping so callers don't silently
        // end up mining with different options than requested. This matches the
        // behavior of the `-blockreservedweight` startup option, which rejects
        // values below MINIMUM_BLOCK_RESERVED_WEIGHT.
        if (options.block_reserved_weight && options.block_reserved_weight < MINIMUM_BLOCK_RESERVED_WEIGHT) {
            throw std::runtime_error(strprintf("block_reserved_weight (%zu) must be at least %u weight units",
                                               *options.block_reserved_weight,
                                               MINIMUM_BLOCK_RESERVED_WEIGHT));
        }

        // Ensure m_tip_block is set so consumers of BlockTemplate can rely on that.
        std::optional<BlockRef> maybe_tip{waitTipChanged(uint256::ZERO, MillisecondsDouble::max())};

        if (!maybe_tip) return {};

        if (cooldown) {
            // Do not return a template during IBD, because it can have long
            // pauses and sometimes takes a while to get started. Although this
            // is useful in general, it's gated behind the cooldown argument,
            // because on regtest and single miner signets this would wait
            // forever if no block was mined in the past day.
            while (chainman().IsInitialBlockDownload()) {
                maybe_tip = waitTipChanged(maybe_tip->hash, MillisecondsDouble{1000});
                if (!maybe_tip || chainman().m_interrupt || WITH_LOCK(notifications().m_tip_block_mutex, return m_interrupt_mining)) return {};
            }

            // Also wait during the final catch-up moments after IBD.
            if (!CooldownIfHeadersAhead(chainman(), notifications(), *maybe_tip, m_interrupt_mining)) return {};
        }

        BlockAssembler::Options assemble_options{options};
        ApplyArgsManOptions(*Assert(m_node.args), assemble_options);
        return std::make_unique<BlockTemplateImpl>(assemble_options, BlockAssembler{chainman().ActiveChainstate(), context()->mempool.get(), assemble_options}.CreateNewBlock(), m_node);
    }

    void interrupt() override
    {
        InterruptWait(notifications(), m_interrupt_mining);
    }

    bool checkBlock(const CBlock& block, const node::BlockCheckOptions& options, std::string& reason, std::string& debug) override
    {
        LOCK(chainman().GetMutex());
        BlockValidationState state{TestBlockValidity(chainman().ActiveChainstate(), block, /*check_pow=*/options.check_pow, /*check_merkle_root=*/options.check_merkle_root)};
        reason = state.GetRejectReason();
        debug = state.GetDebugMessage();
        return state.IsValid();
    }

    NodeContext* context() override { return &m_node; }
    ChainstateManager& chainman() { return *Assert(m_node.chainman); }
    KernelNotifications& notifications() { return *Assert(m_node.notifications); }
    // Treat as if guarded by notifications().m_tip_block_mutex
    bool m_interrupt_mining{false};
    NodeContext& m_node;
};
} // namespace
} // namespace node

namespace interfaces {
std::unique_ptr<Node> MakeNode(node::NodeContext& context) { return std::make_unique<node::NodeImpl>(context); }
std::unique_ptr<Chain> MakeChain(node::NodeContext& context) { return std::make_unique<node::ChainImpl>(context); }
std::unique_ptr<Mining> MakeMining(node::NodeContext& context, bool wait_loaded)
{
    if (wait_loaded) {
        node::KernelNotifications& kernel_notifications(*Assert(context.notifications));
        util::SignalInterrupt& interrupt(*Assert(context.shutdown_signal));
        WAIT_LOCK(kernel_notifications.m_tip_block_mutex, lock);
        kernel_notifications.m_tip_block_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(kernel_notifications.m_tip_block_mutex) {
            return kernel_notifications.m_state.chainstate_loaded || interrupt;
        });
        if (interrupt) return nullptr;
    }
    return std::make_unique<node::MinerImpl>(context);
}
} // namespace interfaces
