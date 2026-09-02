// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/wallet.h>
#include <coins.h>
#include <modern/asset_output.h>
#include <modern/asset_validation.h>
#include <script/interpreter.h>
#include <script/sign.h>
#include <modern/stake.h>

#include <cstdint>
#include <future>
#include <memory>
#include <vector>

#include <addresstype.h>
#include <common/args.h>
#include <interfaces/chain.h>
#include <interfaces/wallet.h>
#include <kernel/mempool_removal_reason.h>
#include <key_io.h>
#include <legacy/consensus.h>
#include <node/blockstorage.h>
#include <node/types.h>
#include <policy/policy.h>
#include <rpc/server.h>
#include <script/solver.h>
#include <test/util/common.h>
#include <test/util/logging.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/receive.h>
#include <wallet/spend.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>

#include <boost/test/unit_test.hpp>
#include <univalue.h>

using node::MAX_BLOCKFILE_SIZE;

namespace wallet {

// Ensure that fee levels defined in the wallet are at least as high
// as the default levels for node policy.
static_assert(DEFAULT_TRANSACTION_MINFEE >= DEFAULT_MIN_RELAY_TX_FEE, "wallet minimum fee is smaller than default relay fee");
static_assert(WALLET_INCREMENTAL_RELAY_FEE >= DEFAULT_INCREMENTAL_RELAY_FEE, "wallet incremental fee is smaller than default incremental relay fee");

BOOST_FIXTURE_TEST_SUITE(wallet_tests, WalletTestingSetup)

static CMutableTransaction TestSimpleSpend(const CTransaction& from, uint32_t index, const CKey& key, const CScript& pubkey)
{
    CMutableTransaction mtx;
    // A plain fee, NOT DEFAULT_TRANSACTION_MAXFEE: with B3's corrected max
    // (0.1 B3 = 1e8 base) the stock expression exceeds a regtest coinbase
    // and produces a negative output.
    mtx.vout.emplace_back(from.vout[index].nValue - 100'000, pubkey);
    mtx.vin.push_back({CTxIn{from.GetHash(), index}});
    FillableSigningProvider keystore;
    keystore.AddKey(key);
    std::map<COutPoint, Coin> coins;
    coins[mtx.vin[0].prevout].out = from.vout[index];
    std::map<int, bilingual_str> input_errors;
    BOOST_CHECK(SignTransaction(mtx, &keystore, coins, SIGHASH_ALL, input_errors));
    return mtx;
}

static void AddKey(CWallet& wallet, const CKey& key)
{
    LOCK(wallet.cs_wallet);
    FlatSigningProvider provider;
    std::string error;
    auto descs = Parse("combo(" + EncodeSecret(key) + ")", provider, error, /* require_checksum=*/ false);
    assert(descs.size() == 1);
    auto& desc = descs.at(0);
    WalletDescriptor w_desc(std::move(desc), 0, 0, 1, 1);
    Assert(wallet.AddWalletDescriptor(w_desc, provider, "", false));
}

BOOST_FIXTURE_TEST_CASE(update_non_range_descriptor, TestingSetup)
{
    CWallet wallet(m_node.chain.get(), "", CreateMockableWalletDatabase());
    {
        LOCK(wallet.cs_wallet);
        wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        auto key{GenerateRandomKey()};
        auto desc_str{"combo(" + EncodeSecret(key) + ")"};
        FlatSigningProvider provider;
        std::string error;
        auto descs{Parse(desc_str, provider, error, /* require_checksum=*/ false)};
        auto& desc{descs.at(0)};
        WalletDescriptor w_desc{std::move(desc), 0, 0, 0, 0};
        BOOST_CHECK(wallet.AddWalletDescriptor(w_desc, provider, "", false));
        // Wallet should update the non-range descriptor successfully
        BOOST_CHECK(wallet.AddWalletDescriptor(w_desc, provider, "", false));
    }
}

BOOST_FIXTURE_TEST_CASE(scan_for_wallet_transactions, TestChain100Setup)
{
    // Cap last block file size, and mine new block in a new block file.
    CBlockIndex* oldTip = WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip());
    WITH_LOCK(::cs_main, m_node.chainman->m_blockman.GetBlockFileInfo(oldTip->GetBlockPos().nFile)->nSize = MAX_BLOCKFILE_SIZE);
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    CBlockIndex* newTip = WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip());

    // Verify ScanForWalletTransactions fails to read an unknown start block.
    {
        CWallet wallet(m_node.chain.get(), "", CreateMockableWalletDatabase());
        {
            LOCK(wallet.cs_wallet);
            LOCK(Assert(m_node.chainman)->GetMutex());
            wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            wallet.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }
        AddKey(wallet, coinbaseKey);
        WalletRescanReserver reserver(wallet);
        reserver.reserve();
        CWallet::ScanResult result = wallet.ScanForWalletTransactions(/*start_block=*/{}, /*start_height=*/0, /*max_height=*/{}, reserver, /*fUpdate=*/false, /*save_progress=*/false);
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::FAILURE);
        BOOST_CHECK(result.last_failed_block.IsNull());
        BOOST_CHECK(result.last_scanned_block.IsNull());
        BOOST_CHECK(!result.last_scanned_height);
        BOOST_CHECK_EQUAL(GetBalance(wallet).m_mine_immature, 0);
    }

    // Verify ScanForWalletTransactions picks up transactions in both the old
    // and new block files.
    {
        CWallet wallet(m_node.chain.get(), "", CreateMockableWalletDatabase());
        {
            LOCK(wallet.cs_wallet);
            LOCK(Assert(m_node.chainman)->GetMutex());
            wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            wallet.SetLastBlockProcessed(newTip->nHeight, newTip->GetBlockHash());
        }
        AddKey(wallet, coinbaseKey);
        WalletRescanReserver reserver(wallet);
        std::chrono::steady_clock::time_point fake_time;
        reserver.setNow([&] { fake_time += 60s; return fake_time; });
        reserver.reserve();

        {
            CBlockLocator locator;
            BOOST_CHECK(WalletBatch{wallet.GetDatabase()}.ReadBestBlock(locator));
            BOOST_CHECK(!locator.IsNull() && locator.vHave.front() == newTip->GetBlockHash());
        }

        CWallet::ScanResult result = wallet.ScanForWalletTransactions(/*start_block=*/oldTip->GetBlockHash(), /*start_height=*/oldTip->nHeight, /*max_height=*/{}, reserver, /*fUpdate=*/false, /*save_progress=*/true);
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::SUCCESS);
        BOOST_CHECK(result.last_failed_block.IsNull());
        BOOST_CHECK_EQUAL(result.last_scanned_block, newTip->GetBlockHash());
        BOOST_CHECK_EQUAL(*result.last_scanned_height, newTip->nHeight);
        BOOST_CHECK_EQUAL(GetBalance(wallet).m_mine_immature, 100 * COIN);

        {
            CBlockLocator locator;
            BOOST_CHECK(WalletBatch{wallet.GetDatabase()}.ReadBestBlock(locator));
            BOOST_CHECK(!locator.IsNull() && locator.vHave.front() == newTip->GetBlockHash());
        }
    }

    // Prune the older block file.
    int file_number;
    {
        LOCK(cs_main);
        file_number = oldTip->GetBlockPos().nFile;
        Assert(m_node.chainman)->m_blockman.PruneOneBlockFile(file_number);
    }
    m_node.chainman->m_blockman.UnlinkPrunedFiles({file_number});

    // Verify ScanForWalletTransactions only picks transactions in the new block
    // file.
    {
        CWallet wallet(m_node.chain.get(), "", CreateMockableWalletDatabase());
        {
            LOCK(wallet.cs_wallet);
            LOCK(Assert(m_node.chainman)->GetMutex());
            wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            wallet.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }
        AddKey(wallet, coinbaseKey);
        WalletRescanReserver reserver(wallet);
        reserver.reserve();
        CWallet::ScanResult result = wallet.ScanForWalletTransactions(/*start_block=*/oldTip->GetBlockHash(), /*start_height=*/oldTip->nHeight, /*max_height=*/{}, reserver, /*fUpdate=*/false, /*save_progress=*/false);
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::FAILURE);
        BOOST_CHECK_EQUAL(result.last_failed_block, oldTip->GetBlockHash());
        BOOST_CHECK_EQUAL(result.last_scanned_block, newTip->GetBlockHash());
        BOOST_CHECK_EQUAL(*result.last_scanned_height, newTip->nHeight);
        BOOST_CHECK_EQUAL(GetBalance(wallet).m_mine_immature, 50 * COIN);
    }

    // Prune the remaining block file.
    {
        LOCK(cs_main);
        file_number = newTip->GetBlockPos().nFile;
        Assert(m_node.chainman)->m_blockman.PruneOneBlockFile(file_number);
    }
    m_node.chainman->m_blockman.UnlinkPrunedFiles({file_number});

    // Verify ScanForWalletTransactions scans no blocks.
    {
        CWallet wallet(m_node.chain.get(), "", CreateMockableWalletDatabase());
        {
            LOCK(wallet.cs_wallet);
            LOCK(Assert(m_node.chainman)->GetMutex());
            wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            wallet.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }
        AddKey(wallet, coinbaseKey);
        WalletRescanReserver reserver(wallet);
        reserver.reserve();
        CWallet::ScanResult result = wallet.ScanForWalletTransactions(/*start_block=*/oldTip->GetBlockHash(), /*start_height=*/oldTip->nHeight, /*max_height=*/{}, reserver, /*fUpdate=*/false, /*save_progress=*/false);
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::FAILURE);
        BOOST_CHECK_EQUAL(result.last_failed_block, newTip->GetBlockHash());
        BOOST_CHECK(result.last_scanned_block.IsNull());
        BOOST_CHECK(!result.last_scanned_height);
        BOOST_CHECK_EQUAL(GetBalance(wallet).m_mine_immature, 0);
    }
}

// This test verifies that wallet settings can be added and removed
// concurrently, ensuring no race conditions occur during either process.
BOOST_FIXTURE_TEST_CASE(write_wallet_settings_concurrently, TestingSetup)
{
    auto chain = m_node.chain.get();
    const auto NUM_WALLETS{5};

    // Since we're counting the number of wallets, ensure we start without any.
    BOOST_REQUIRE(chain->getRwSetting("wallet").isNull());

    const auto& check_concurrent_wallet = [&](const auto& settings_function, int num_expected_wallets) {
        std::vector<std::thread> threads;
        threads.reserve(NUM_WALLETS);
        for (auto i{0}; i < NUM_WALLETS; ++i) threads.emplace_back(settings_function, i);
        for (auto& t : threads) t.join();

        auto wallets = chain->getRwSetting("wallet");
        BOOST_CHECK_EQUAL(wallets.getValues().size(), num_expected_wallets);
    };

    // Add NUM_WALLETS wallets concurrently, ensure we end up with NUM_WALLETS stored.
    check_concurrent_wallet([&chain](int i) {
        Assert(AddWalletSetting(*chain, strprintf("wallet_%d", i)));
    },
                            /*num_expected_wallets=*/NUM_WALLETS);

    // Remove NUM_WALLETS wallets concurrently, ensure we end up with 0 wallets.
    check_concurrent_wallet([&chain](int i) {
        Assert(RemoveWalletSetting(*chain, strprintf("wallet_%d", i)));
    },
                            /*num_expected_wallets=*/0);
}

static int64_t AddTx(ChainstateManager& chainman, CWallet& wallet, uint32_t lockTime, int64_t mockTime, int64_t blockTime)
{
    CMutableTransaction tx;
    TxState state = TxStateInactive{};
    tx.nLockTime = lockTime;
    SetMockTime(mockTime);
    CBlockIndex* block = nullptr;
    if (blockTime > 0) {
        LOCK(cs_main);
        auto inserted = chainman.BlockIndex().emplace(std::piecewise_construct, std::make_tuple(GetRandHash()), std::make_tuple());
        assert(inserted.second);
        const uint256& hash = inserted.first->first;
        block = &inserted.first->second;
        block->nTime = blockTime;
        block->phashBlock = &hash;
        state = TxStateConfirmed{hash, block->nHeight, /*index=*/0};
    }
    return wallet.AddToWallet(MakeTransactionRef(tx), state, [&](CWalletTx& wtx, bool /* new_tx */) {
        // Assign wtx.m_state to simplify test and avoid the need to simulate
        // reorg events. Without this, AddToWallet asserts false when the same
        // transaction is confirmed in different blocks.
        wtx.m_state = state;
        return true;
    })->nTimeSmart;
}

// Simple test to verify assignment of CWalletTx::nSmartTime value. Could be
// expanded to cover more corner cases of smart time logic.
BOOST_AUTO_TEST_CASE(ComputeTimeSmart)
{
    // New transaction should use clock time if lower than block time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_wallet, 1, 100, 120), 100);

    // Test that updating existing transaction does not change smart time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_wallet, 1, 200, 220), 100);

    // New transaction should use clock time if there's no block time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_wallet, 2, 300, 0), 300);

    // New transaction should use block time if lower than clock time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_wallet, 3, 420, 400), 400);

    // New transaction should use latest entry time if higher than
    // min(block time, clock time).
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_wallet, 4, 500, 390), 400);

    // If there are future entries, new transaction should use time of the
    // newest entry that is no more than 300 seconds ahead of the clock time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_wallet, 5, 50, 600), 300);
}

void TestLoadWallet(const std::string& name, DatabaseFormat format, std::function<void(std::shared_ptr<CWallet>)> f)
{
    node::NodeContext node;
    auto chain{interfaces::MakeChain(node)};
    DatabaseOptions options;
    options.require_format = format;
    DatabaseStatus status;
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    auto database{MakeWalletDatabase(name, options, status, error)};
    auto wallet{std::make_shared<CWallet>(chain.get(), "", std::move(database))};
    BOOST_CHECK_EQUAL(wallet->PopulateWalletFromDB(error, warnings), DBErrors::LOAD_OK);
    WITH_LOCK(wallet->cs_wallet, f(wallet));
}

BOOST_FIXTURE_TEST_CASE(LoadReceiveRequests, TestingSetup)
{
    for (DatabaseFormat format : DATABASE_FORMATS) {
        const std::string name{strprintf("receive-requests-%i", format)};
        TestLoadWallet(name, format, [](std::shared_ptr<CWallet> wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet->cs_wallet) {
            BOOST_CHECK(!wallet->IsAddressPreviouslySpent(PKHash()));
            WalletBatch batch{wallet->GetDatabase()};
            BOOST_CHECK(batch.WriteAddressPreviouslySpent(PKHash(), true));
            BOOST_CHECK(batch.WriteAddressPreviouslySpent(ScriptHash(), true));
            BOOST_CHECK(wallet->SetAddressReceiveRequest(batch, PKHash(), "0", "val_rr00"));
            BOOST_CHECK(wallet->EraseAddressReceiveRequest(batch, PKHash(), "0"));
            BOOST_CHECK(wallet->SetAddressReceiveRequest(batch, PKHash(), "1", "val_rr10"));
            BOOST_CHECK(wallet->SetAddressReceiveRequest(batch, PKHash(), "1", "val_rr11"));
            BOOST_CHECK(wallet->SetAddressReceiveRequest(batch, ScriptHash(), "2", "val_rr20"));
        });
        TestLoadWallet(name, format, [](std::shared_ptr<CWallet> wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet->cs_wallet) {
            BOOST_CHECK(wallet->IsAddressPreviouslySpent(PKHash()));
            BOOST_CHECK(wallet->IsAddressPreviouslySpent(ScriptHash()));
            auto requests = wallet->GetAddressReceiveRequests();
            auto erequests = {"val_rr11", "val_rr20"};
            BOOST_CHECK_EQUAL_COLLECTIONS(requests.begin(), requests.end(), std::begin(erequests), std::end(erequests));
            RunWithinTxn(wallet->GetDatabase(), /*process_desc=*/"test", [](WalletBatch& batch){
                BOOST_CHECK(batch.WriteAddressPreviouslySpent(PKHash(), false));
                BOOST_CHECK(batch.EraseAddressData(ScriptHash()));
                return true;
            });
        });
        TestLoadWallet(name, format, [](std::shared_ptr<CWallet> wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet->cs_wallet) {
            BOOST_CHECK(!wallet->IsAddressPreviouslySpent(PKHash()));
            BOOST_CHECK(!wallet->IsAddressPreviouslySpent(ScriptHash()));
            auto requests = wallet->GetAddressReceiveRequests();
            auto erequests = {"val_rr11"};
            BOOST_CHECK_EQUAL_COLLECTIONS(requests.begin(), requests.end(), std::begin(erequests), std::end(erequests));
        });
    }
}

class ListCoinsTestingSetup : public TestChain100Setup
{
public:
    ListCoinsTestingSetup()
    {
        CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
        wallet = CreateSyncedWallet(*m_node.chain, WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()), coinbaseKey);
    }

    ~ListCoinsTestingSetup()
    {
        wallet.reset();
    }

    CWalletTx& AddTx(CRecipient recipient)
    {
        CTransactionRef tx;
        CCoinControl dummy;
        {
            auto res = CreateTransaction(*wallet, {recipient}, /*change_pos=*/std::nullopt, dummy);
            BOOST_CHECK(res);
            tx = res->tx;
        }
        wallet->CommitTransaction(tx, {}, {});
        CMutableTransaction blocktx;
        {
            LOCK(wallet->cs_wallet);
            blocktx = CMutableTransaction(*wallet->mapWallet.at(tx->GetHash()).tx);
        }
        CreateAndProcessBlock({CMutableTransaction(blocktx)}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

        LOCK(wallet->cs_wallet);
        LOCK(Assert(m_node.chainman)->GetMutex());
        wallet->SetLastBlockProcessed(wallet->GetLastBlockHeight() + 1, m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        auto it = wallet->mapWallet.find(tx->GetHash());
        BOOST_CHECK(it != wallet->mapWallet.end());
        it->second.m_state = TxStateConfirmed{m_node.chainman->ActiveChain().Tip()->GetBlockHash(), m_node.chainman->ActiveChain().Height(), /*index=*/1};
        return it->second;
    }

    std::unique_ptr<CWallet> wallet;
};

BOOST_FIXTURE_TEST_CASE(ListCoinsTest, ListCoinsTestingSetup)
{
    std::string coinbaseAddress = coinbaseKey.GetPubKey().GetID().ToString();

    // Confirm ListCoins initially returns 1 coin grouped under coinbaseKey
    // address.
    std::map<CTxDestination, std::vector<COutput>> list;
    {
        LOCK(wallet->cs_wallet);
        list = ListCoins(*wallet);
    }
    BOOST_CHECK_EQUAL(list.size(), 1U);
    BOOST_CHECK_EQUAL(std::get<PKHash>(list.begin()->first).ToString(), coinbaseAddress);
    BOOST_CHECK_EQUAL(list.begin()->second.size(), 1U);

    // Check initial balance from one mature coinbase transaction.
    BOOST_CHECK_EQUAL(50 * COIN, WITH_LOCK(wallet->cs_wallet, return AvailableCoins(*wallet).GetTotalAmount()));

    // Add a transaction creating a change address, and confirm ListCoins still
    // returns the coin associated with the change address underneath the
    // coinbaseKey pubkey, even though the change address has a different
    // pubkey.
    AddTx(CRecipient{PubKeyDestination{{}}, 1 * COIN, /*subtract_fee=*/false});
    {
        LOCK(wallet->cs_wallet);
        list = ListCoins(*wallet);
    }
    BOOST_CHECK_EQUAL(list.size(), 1U);
    BOOST_CHECK_EQUAL(std::get<PKHash>(list.begin()->first).ToString(), coinbaseAddress);
    BOOST_CHECK_EQUAL(list.begin()->second.size(), 2U);

    // Lock both coins. Confirm number of available coins drops to 0.
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK_EQUAL(AvailableCoins(*wallet).Size(), 2U);
    }
    for (const auto& group : list) {
        for (const auto& coin : group.second) {
            LOCK(wallet->cs_wallet);
            wallet->LockCoin(coin.outpoint, /*persist=*/false);
        }
    }
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK_EQUAL(AvailableCoins(*wallet).Size(), 0U);
    }
    // Confirm ListCoins still returns same result as before, despite coins
    // being locked.
    {
        LOCK(wallet->cs_wallet);
        list = ListCoins(*wallet);
    }
    BOOST_CHECK_EQUAL(list.size(), 1U);
    BOOST_CHECK_EQUAL(std::get<PKHash>(list.begin()->first).ToString(), coinbaseAddress);
    BOOST_CHECK_EQUAL(list.begin()->second.size(), 2U);
}

void TestCoinsResult(ListCoinsTest& context, OutputType out_type, CAmount amount,
                     std::map<OutputType, size_t>& expected_coins_sizes)
{
    LOCK(context.wallet->cs_wallet);
    util::Result<CTxDestination> dest = Assert(context.wallet->GetNewDestination(out_type, ""));
    CWalletTx& wtx = context.AddTx(CRecipient{*dest, amount, /*fSubtractFeeFromAmount=*/true});
    CoinFilterParams filter;
    filter.skip_locked = false;
    CoinsResult available_coins = AvailableCoins(*context.wallet, nullptr, std::nullopt, filter);
    // Lock outputs so they are not spent in follow-up transactions
    for (uint32_t i = 0; i < wtx.tx->vout.size(); i++) context.wallet->LockCoin({wtx.GetHash(), i}, /*persist=*/false);
    for (const auto& [type, size] : expected_coins_sizes) BOOST_CHECK_EQUAL(size, available_coins.coins[type].size());
}

BOOST_FIXTURE_TEST_CASE(BasicOutputTypesTest, ListCoinsTest)
{
    std::map<OutputType, size_t> expected_coins_sizes;
    for (const auto& out_type : OUTPUT_TYPES) { expected_coins_sizes[out_type] = 0U; }

    // Verify our wallet has one usable coinbase UTXO before starting
    // This UTXO is a P2PK, so it should show up in the Other bucket
    expected_coins_sizes[OutputType::UNKNOWN] = 1U;
    CoinsResult available_coins = WITH_LOCK(wallet->cs_wallet, return AvailableCoins(*wallet));
    BOOST_CHECK_EQUAL(available_coins.Size(), expected_coins_sizes[OutputType::UNKNOWN]);
    BOOST_CHECK_EQUAL(available_coins.coins[OutputType::UNKNOWN].size(), expected_coins_sizes[OutputType::UNKNOWN]);

    // We will create a self transfer for each of the OutputTypes and
    // verify it is put in the correct bucket after running GetAvailablecoins
    //
    // For each OutputType, We expect 2 UTXOs in our wallet following the self transfer:
    //   1. One UTXO as the recipient
    //   2. One UTXO from the change, due to payment address matching logic

    for (const auto& out_type : OUTPUT_TYPES) {
        if (out_type == OutputType::UNKNOWN) continue;
        expected_coins_sizes[out_type] = 2U;
        TestCoinsResult(*this, out_type, 1 * COIN, expected_coins_sizes);
    }
}

BOOST_FIXTURE_TEST_CASE(wallet_disableprivkeys, TestChain100Setup)
{
    const std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(m_node.chain.get(), "", CreateMockableWalletDatabase());
    LOCK(wallet->cs_wallet);
    wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    BOOST_CHECK(!wallet->GetNewDestination(OutputType::BECH32, ""));
}

// Explicit calculation which is used to test the wallet constant
// We get the same virtual size due to rounding(weight/4) for both use_max_sig values
static size_t CalculateNestedKeyhashInputSize(bool use_max_sig)
{
    // Generate ephemeral valid pubkey
    CKey key = GenerateRandomKey();
    CPubKey pubkey = key.GetPubKey();

    // Generate pubkey hash
    uint160 key_hash(Hash160(pubkey));

    // Create inner-script to enter into keystore. Key hash can't be 0...
    CScript inner_script = CScript() << OP_0 << std::vector<unsigned char>(key_hash.begin(), key_hash.end());

    // Create outer P2SH script for the output
    uint160 script_id(Hash160(inner_script));
    CScript script_pubkey = CScript() << OP_HASH160 << std::vector<unsigned char>(script_id.begin(), script_id.end()) << OP_EQUAL;

    // Add inner-script to key store and key to watchonly
    FillableSigningProvider keystore;
    keystore.AddCScript(inner_script);
    keystore.AddKeyPubKey(key, pubkey);

    // Fill in dummy signatures for fee calculation.
    SignatureData sig_data;

    if (!ProduceSignature(keystore, use_max_sig ? DUMMY_MAXIMUM_SIGNATURE_CREATOR : DUMMY_SIGNATURE_CREATOR, script_pubkey, sig_data)) {
        // We're hand-feeding it correct arguments; shouldn't happen
        assert(false);
    }

    CTxIn tx_in;
    UpdateInput(tx_in, sig_data);
    return (size_t)GetVirtualTransactionInputSize(tx_in);
}

BOOST_FIXTURE_TEST_CASE(dummy_input_size_test, TestChain100Setup)
{
    BOOST_CHECK_EQUAL(CalculateNestedKeyhashInputSize(false), DUMMY_NESTED_P2WPKH_INPUT_SIZE);
    BOOST_CHECK_EQUAL(CalculateNestedKeyhashInputSize(true), DUMMY_NESTED_P2WPKH_INPUT_SIZE);
}

bool malformed_descriptor(std::ios_base::failure e)
{
    std::string s(e.what());
    return s.find("Missing checksum") != std::string::npos;
}

BOOST_FIXTURE_TEST_CASE(wallet_descriptor_test, BasicTestingSetup)
{
    std::vector<unsigned char> malformed_record;
    VectorWriter vw{malformed_record, 0};
    vw << std::string("notadescriptor");
    vw << uint64_t{0};
    vw << int32_t{0};
    vw << int32_t{0};
    vw << int32_t{1};

    SpanReader vr{malformed_record};
    WalletDescriptor w_desc;
    BOOST_CHECK_EXCEPTION(vr >> w_desc, std::ios_base::failure, malformed_descriptor);
}

//! Test CWallet::CreateNew() and its behavior handling potential race
//! conditions if it's called the same time an incoming transaction shows up in
//! the mempool or a new block.
//!
//! It isn't possible to verify there aren't race condition in every case, so
//! this test just checks two specific cases and ensures that timing of
//! notifications in these cases doesn't prevent the wallet from detecting
//! transactions.
//!
//! In the first case, block and mempool transactions are created before the
//! wallet is loaded, but notifications about these transactions are delayed
//! until after it is loaded. The notifications are superfluous in this case, so
//! the test verifies the transactions are detected before they arrive.
//!
//! In the second case, block and mempool transactions are created after the
//! wallet rescan and notifications are immediately synced, to verify the wallet
//! must already have a handler in place for them, and there's no gap after
//! rescanning where new transactions in new blocks could be lost.
BOOST_FIXTURE_TEST_CASE(CreateWallet, TestChain100Setup)
{
    m_args.ForceSetArg("-unsafesqlitesync", "1");
    // Create new wallet with known key and unload it.
    WalletContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    auto wallet = TestCreateWallet(context);
    CKey key = GenerateRandomKey();
    AddKey(*wallet, key);
    TestUnloadWallet(std::move(wallet));


    // Add log hook to detect AddToWallet events from rescans, blockConnected,
    // and transactionAddedToMempool notifications
    int addtx_count = 0;
    DebugLogHelper addtx_counter("[default wallet] AddToWallet", [&](const std::string* s) {
        if (s) ++addtx_count;
        return false;
    });


    bool rescan_completed = false;
    DebugLogHelper rescan_check("[default wallet] Rescan completed", [&](const std::string* s) {
        if (s) rescan_completed = true;
        return false;
    });


    // Block the queue to prevent the wallet receiving blockConnected and
    // transactionAddedToMempool notifications, and create block and mempool
    // transactions paying to the wallet
    std::promise<void> promise;
    m_node.validation_signals->CallFunctionInValidationInterfaceQueue([&promise] {
        promise.get_future().wait();
    });
    std::string error;
    m_coinbase_txns.push_back(CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
    auto block_tx = TestSimpleSpend(*m_coinbase_txns[0], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
    m_coinbase_txns.push_back(CreateAndProcessBlock({block_tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
    auto mempool_tx = TestSimpleSpend(*m_coinbase_txns[1], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
    BOOST_CHECK(m_node.chain->broadcastTransaction(MakeTransactionRef(mempool_tx), DEFAULT_TRANSACTION_MAXFEE, node::TxBroadcast::MEMPOOL_NO_BROADCAST, error));


    // Reload wallet and make sure new transactions are detected despite events
    // being blocked
    // Loading will also ask for current mempool transactions
    wallet = TestLoadWallet(context);
    BOOST_CHECK(rescan_completed);
    // AddToWallet events for block_tx and mempool_tx (x2)
    BOOST_CHECK_EQUAL(addtx_count, 3);
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->mapWallet.contains(block_tx.GetHash()));
        BOOST_CHECK(wallet->mapWallet.contains(mempool_tx.GetHash()));
    }


    // Unblock notification queue and make sure stale blockConnected and
    // transactionAddedToMempool events are processed
    promise.set_value();
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    // AddToWallet events for block_tx and mempool_tx events are counted a
    // second time as the notification queue is processed
    BOOST_CHECK_EQUAL(addtx_count, 5);


    TestUnloadWallet(std::move(wallet));


    // Load wallet again, this time creating new block and mempool transactions
    // paying to the wallet as the wallet finishes loading and syncing the
    // queue so the events have to be handled immediately. Releasing the wallet
    // lock during the sync is a little artificial but is needed to avoid a
    // deadlock during the sync and simulates a new block notification happening
    // as soon as possible.
    addtx_count = 0;
    auto handler = HandleLoadWallet(context, [&](std::unique_ptr<interfaces::Wallet> wallet) {
            BOOST_CHECK(rescan_completed);
            m_coinbase_txns.push_back(CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
            block_tx = TestSimpleSpend(*m_coinbase_txns[2], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
            m_coinbase_txns.push_back(CreateAndProcessBlock({block_tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
            mempool_tx = TestSimpleSpend(*m_coinbase_txns[3], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
            BOOST_CHECK(m_node.chain->broadcastTransaction(MakeTransactionRef(mempool_tx), DEFAULT_TRANSACTION_MAXFEE, node::TxBroadcast::MEMPOOL_NO_BROADCAST, error));
            m_node.validation_signals->SyncWithValidationInterfaceQueue();
        });
    wallet = TestLoadWallet(context);
    // Since mempool transactions are requested at the end of loading, there will
    // be 2 additional AddToWallet calls, one from the previous test, and a duplicate for mempool_tx
    BOOST_CHECK_EQUAL(addtx_count, 2 + 2);
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->mapWallet.contains(block_tx.GetHash()));
        BOOST_CHECK(wallet->mapWallet.contains(mempool_tx.GetHash()));
    }


    TestUnloadWallet(std::move(wallet));
}

BOOST_FIXTURE_TEST_CASE(CreateWalletWithoutChain, BasicTestingSetup)
{
    WalletContext context;
    context.args = &m_args;
    auto wallet = TestCreateWallet(context);
    BOOST_CHECK(wallet);
    WaitForDeleteWallet(std::move(wallet));
}

BOOST_AUTO_TEST_CASE(b3_blank_recovery_wallet_uses_spendable_legacy_defaults)
{
    // Deliberately bypass WalletInit::ParameterInteraction. Migration and
    // recovery loaders must preserve the default-address invariant themselves.
    WalletContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    auto wallet{TestCreateWallet(
        CreateMockableWalletDatabase(), context,
        WALLET_FLAG_DESCRIPTORS | WALLET_FLAG_BLANK_WALLET)};
    BOOST_REQUIRE(wallet);
    BOOST_CHECK(wallet->m_default_address_type == OutputType::LEGACY);
    BOOST_REQUIRE(wallet->m_default_change_type.has_value());
    BOOST_CHECK(*wallet->m_default_change_type == OutputType::LEGACY);

    // This is the same operation importlegacywalletdump performs when a blank
    // recovery wallet has no active descriptor chains.
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->GetActiveScriptPubKeyMans().empty());
        wallet->SetupDescriptorScriptPubKeyMans();
    }

    const auto receive{
        wallet->GetNewDestination(wallet->m_default_address_type, "recovery")};
    const auto change{wallet->GetNewChangeDestination(
        wallet->m_default_change_type.value())};
    BOOST_REQUIRE(receive);
    BOOST_REQUIRE(change);
    BOOST_CHECK(std::holds_alternative<PKHash>(*receive));
    BOOST_CHECK(std::holds_alternative<PKHash>(*change));

    const auto witness{wallet->GetNewDestination(OutputType::BECH32, "unsafe")};
    BOOST_CHECK(!witness);
    BOOST_CHECK(util::ErrorString(witness).original.find(
                    "witness addresses are not active") != std::string::npos);

    // The descriptor created for the default legacy chain can actually sign
    // its recovered wallet's next spend; this catches a receive-only fix which
    // would still leave bind/funding RPCs unable to create change.
    CMutableTransaction parent;
    parent.vin.emplace_back(
        COutPoint{Txid::FromUint256(uint256::ONE), 0});
    parent.vout.emplace_back(2 * COIN, GetScriptForDestination(*receive));
    const CTransactionRef parent_ref{MakeTransactionRef(parent)};
    CMutableTransaction spend;
    spend.vin.emplace_back(COutPoint{parent_ref->GetHash(), 0});
    spend.vout.emplace_back(COIN, GetScriptForDestination(*change));
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(wallet->AddToWallet(parent_ref, TxStateInactive{}));
        BOOST_CHECK(WalletCanSpendScriptNow(
            *wallet, parent_ref->vout[0].scriptPubKey));
        BOOST_REQUIRE(wallet->SignTransaction(spend));
    }
    BOOST_CHECK(!spend.vin[0].scriptSig.empty());
    BOOST_CHECK(spend.vin[0].scriptWitness.IsNull());

    // An explicit unsafe default is rejected even when the caller bypassed
    // the node-wide parameter interaction.
    ArgsManager explicit_args;
    explicit_args.ForceSetArg("-addresstype", "bech32");
    WalletContext explicit_context;
    explicit_context.args = &explicit_args;
    explicit_context.chain = m_node.chain.get();
    auto rejected_wallet{std::make_shared<CWallet>(
        m_node.chain.get(), "", CreateMockableWalletDatabase())};
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    BOOST_CHECK(!CWallet::LoadWalletArgs(
        rejected_wallet, explicit_context, error, warnings));
    BOOST_CHECK(error.original.find("witness addresses are not active") !=
                std::string::npos);

    // Outputs created by an older unsafe default stay visible for recovery,
    // but automatic coin selection must not build an unconfirmable spend.
    CTransactionRef old_witness_funds;
    {
        LOCK(wallet->cs_wallet);
        ScriptPubKeyMan* witness_manager{
            wallet->GetScriptPubKeyMan(OutputType::BECH32,
                                       /*internal=*/false)};
        ScriptPubKeyMan* wrapped_manager{
            wallet->GetScriptPubKeyMan(OutputType::P2SH_SEGWIT,
                                       /*internal=*/false)};
        BOOST_REQUIRE(witness_manager != nullptr);
        BOOST_REQUIRE(wrapped_manager != nullptr);
        const auto old_destination{
            witness_manager->GetNewDestination(OutputType::BECH32)};
        const auto old_wrapped_destination{
            wrapped_manager->GetNewDestination(OutputType::P2SH_SEGWIT)};
        BOOST_REQUIRE(old_destination);
        BOOST_REQUIRE(old_wrapped_destination);
        CMutableTransaction old_parent;
        old_parent.vin.emplace_back(
            COutPoint{Txid::FromUint256(uint256{2}), 0});
        old_parent.vout.emplace_back(
            2 * COIN, GetScriptForDestination(*old_destination));
        old_parent.vout.emplace_back(
            3 * COIN, GetScriptForDestination(*old_wrapped_destination));
        old_witness_funds = MakeTransactionRef(old_parent);
        const uint256 genesis{m_node.chain->getBlockHash(0)};
        BOOST_REQUIRE(wallet->AddToWallet(
            old_witness_funds,
            TxStateConfirmed{genesis, /*height=*/0, /*index=*/1}));
        wallet->SetLastBlockProcessed(/*height=*/0, genesis);

        const CoinsResult visible{AvailableCoins(*wallet)};
        BOOST_REQUIRE_EQUAL(visible.Size(), 2U);
        for (uint32_t vout{0}; vout < 2; ++vout) {
            const COutPoint outpoint{old_witness_funds->GetHash(), vout};
            BOOST_CHECK(std::ranges::any_of(
                visible.All(), [&](const COutput& output) {
                    return output.outpoint == outpoint;
                }));

            CCoinControl selected_control;
            selected_control.Select(outpoint);
            FastRandomContext rng{/*fDeterministic=*/true};
            CoinSelectionParams params{rng};
            const auto selected{FetchSelectedInputs(
                *wallet, selected_control, params)};
            BOOST_CHECK(!selected);
            BOOST_CHECK(util::ErrorString(selected).original.find(
                            "witness addresses are not active") !=
                        std::string::npos);
        }

        // Direct bech32 recipients fail before selection and tell the caller
        // to supply a legacy recipient address.
        CCoinControl recipient_control;
        const auto unsafe_recipient{CreateTransaction(
            *wallet,
            {{*old_destination, COIN,
              /*fSubtractFeeFromAmount=*/false}},
            /*change_pos=*/std::nullopt, recipient_control)};
        BOOST_CHECK(!unsafe_recipient);
        BOOST_CHECK(util::ErrorString(unsafe_recipient).original.find(
                        "Use a legacy recipient address") !=
                    std::string::npos);

        CCoinControl coin_control;
        const auto created{CreateTransaction(
            *wallet,
            {{*receive, COIN, /*fSubtractFeeFromAmount=*/false}},
            /*change_pos=*/std::nullopt, coin_control)};
        BOOST_CHECK(!created);
        BOOST_CHECK(util::ErrorString(created).original.find(
                        "witness addresses are not active") !=
                    std::string::npos);
    }

    TestUnloadWallet(std::move(wallet));
}

BOOST_FIXTURE_TEST_CASE(RemoveTxs, TestChain100Setup)
{
    m_args.ForceSetArg("-unsafesqlitesync", "1");
    WalletContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    auto wallet = TestCreateWallet(context);
    CKey key = GenerateRandomKey();
    AddKey(*wallet, key);

    std::string error;
    m_coinbase_txns.push_back(CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
    auto block_tx = TestSimpleSpend(*m_coinbase_txns[0], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
    CreateAndProcessBlock({block_tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

    m_node.validation_signals->SyncWithValidationInterfaceQueue();

    {
        auto block_hash = block_tx.GetHash();
        auto prev_tx = m_coinbase_txns[0];

        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->HasWalletSpend(prev_tx));
        BOOST_CHECK(wallet->mapWallet.contains(block_hash));

        std::vector<Txid> vHashIn{ block_hash };
        BOOST_CHECK(wallet->RemoveTxs(vHashIn));

        BOOST_CHECK(!wallet->HasWalletSpend(prev_tx));
        BOOST_CHECK(!wallet->mapWallet.contains(block_hash));
    }

    TestUnloadWallet(std::move(wallet));
}

//! B3 validator key + STAKE outputs (release-v1 validator UX): the wallet
//! creates and persists one validator key; a STAKE carrier whose bare owner
//! script is ours IS ours (standard, solvable, signable = unstakeable), a
//! carrier with a witness owner is not; and STAKE outputs are never
//! auto-selected for ordinary spends.
BOOST_FIXTURE_TEST_CASE(b3_validator_key_and_stake_outputs, TestChain100Setup)
{
    std::unique_ptr<CWallet> wallet;
    {
        LOCK(cs_main);
        wallet = CreateSyncedWallet(*m_node.chain, m_node.chainman->ActiveChain(), coinbaseKey);
    }
    const CTxDestination owner{PKHash(coinbaseKey.GetPubKey())};
    std::array<unsigned char, 32> validator_id{};
    validator_id.fill(0x42);
    const CScript stake_script{modern::MakeStakeScript(validator_id, GetScriptForDestination(owner))};
    const CScript witness_owner_stake{modern::MakeStakeScript(validator_id, GetScriptForDestination(WitnessV0KeyHash(coinbaseKey.GetPubKey())))};

    {
        LOCK(wallet->cs_wallet);
        // Validator key: created once, recorded, secret recoverable.
        BOOST_CHECK(!wallet->GetValidatorPubKey().has_value());
        const auto created{wallet->GetOrCreateValidatorKey()};
        BOOST_REQUIRE_MESSAGE(created, util::ErrorString(created).original);
        BOOST_CHECK(wallet->GetValidatorPubKey() == *created);
        const auto again{wallet->GetOrCreateValidatorKey()};
        BOOST_REQUIRE(again);
        BOOST_CHECK(*again == *created);
        const auto secret{wallet->GetValidatorSecret()};
        BOOST_REQUIRE_MESSAGE(secret, util::ErrorString(secret).original);
        BOOST_CHECK(secret->GetPubKey() == *created);

        // The trading account is a separate BIP340 identity and persists via
        // its own hidden pk() descriptor.
        BOOST_CHECK(!wallet->GetFlowMeshAccountPubKey());
        const auto flowmesh_account{wallet->GetOrCreateFlowMeshAccountKey()};
        BOOST_REQUIRE_MESSAGE(flowmesh_account,
                              util::ErrorString(flowmesh_account).original);
        BOOST_CHECK(*flowmesh_account != *created);
        BOOST_CHECK(wallet->GetFlowMeshAccountPubKey() == *flowmesh_account);
        const auto flowmesh_account_again{
            wallet->GetOrCreateFlowMeshAccountKey()};
        BOOST_REQUIRE(flowmesh_account_again);
        BOOST_CHECK(*flowmesh_account_again == *flowmesh_account);
        {
            const auto account_secret{wallet->GetFlowMeshAccountSecret()};
            BOOST_REQUIRE_MESSAGE(account_secret,
                                  util::ErrorString(account_secret).original);
            BOOST_CHECK(account_secret->GetPubKey() == *flowmesh_account);
        }

        // BLS finality-key derivation (Commit 17): deterministic per (identity,
        // seq), distinct across sequences, re-derivable after any restore --
        // nothing but the identity key is ever stored.
        const auto bls0{wallet->DeriveFinalityBlsKey(0)};
        BOOST_REQUIRE_MESSAGE(bls0, util::ErrorString(bls0).original);
        const auto bls0_again{wallet->DeriveFinalityBlsKey(0)};
        BOOST_REQUIRE(bls0_again);
        BOOST_CHECK(bls0->GetPublicKey().Compressed() == bls0_again->GetPublicKey().Compressed());
        const auto bls1{wallet->DeriveFinalityBlsKey(1)};
        BOOST_REQUIRE(bls1);
        BOOST_CHECK(bls0->GetPublicKey().Compressed() != bls1->GetPublicKey().Compressed());

        // Independent (imported) BLS key: stored via the wallet's ordinary
        // descriptor machinery; one resolution rule covers derived + imported.
        BOOST_CHECK(!wallet->HasImportedFinalityBlsKey());
        std::array<unsigned char, 32> ikm{};
        ikm.fill(0x5A);
        const auto independent{bls::SecretKey::FromIKM(ikm)};
        BOOST_REQUIRE(independent);
        {
            const auto imported{wallet->ImportFinalityBlsKey(*independent)};
            BOOST_REQUIRE_MESSAGE(imported, util::ErrorString(imported).original);
            BOOST_CHECK(imported->Compressed() == independent->GetPublicKey().Compressed());
        }
        BOOST_CHECK(wallet->HasImportedFinalityBlsKey());
        {
            const auto back{wallet->GetImportedFinalityBlsKey()};
            BOOST_REQUIRE_MESSAGE(back, util::ErrorString(back).original);
            BOOST_CHECK(back->GetPublicKey().Compressed() == independent->GetPublicKey().Compressed());
            BOOST_CHECK(back->Bytes() == independent->Bytes());
        }
        // Fresh bind: the imported key takes precedence over the derivation.
        {
            const auto fresh{wallet->ResolveFinalityBlsKey(0, nullptr)};
            BOOST_REQUIRE(fresh);
            BOOST_CHECK(fresh->GetPublicKey().Compressed() == independent->GetPublicKey().Compressed());
        }
        // Existing binding: whichever wallet key matches its public key.
        const auto to_vec{[](const auto& pk) { return std::vector<unsigned char>(pk.begin(), pk.end()); }};
        {
            const auto bound_derived{to_vec(bls0->GetPublicKey().Compressed())};
            const auto r{wallet->ResolveFinalityBlsKey(0, &bound_derived)};
            BOOST_REQUIRE(r);
            BOOST_CHECK(r->GetPublicKey().Compressed() == bls0->GetPublicKey().Compressed());
        }
        {
            const auto bound_imported{to_vec(independent->GetPublicKey().Compressed())};
            const auto r{wallet->ResolveFinalityBlsKey(7, &bound_imported)};
            BOOST_REQUIRE(r);
            BOOST_CHECK(r->GetPublicKey().Compressed() == independent->GetPublicKey().Compressed());
        }
        {
            std::vector<unsigned char> foreign(48, 0xEE);
            BOOST_CHECK(!wallet->ResolveFinalityBlsKey(0, &foreign));
        }

        // ISOLATION (release-qualification audit): the imported scalar is
        // opaque wallet state -- its secp interpretation is never a
        // descriptor, never IsMine, never a signing or export path.
        {
            CKey shadow;
            const auto secret_bytes{independent->Bytes()};
            shadow.Set(secret_bytes.begin(), secret_bytes.end(), /*fCompressedIn=*/true);
            BOOST_REQUIRE(shadow.IsValid());
            BOOST_CHECK(!wallet->IsMine(CTxOut{COIN, GetScriptForRawPubKey(shadow.GetPubKey())}));
            BOOST_CHECK(!wallet->IsMine(CTxOut{COIN, GetScriptForDestination(PKHash(shadow.GetPubKey()))}));
            BOOST_CHECK(wallet->GetWalletDescriptors(GetScriptForRawPubKey(shadow.GetPubKey())).empty());
        }
        // FlowMesh seat keys are a separate multi-key collection. A wallet
        // can operate more than one FN seat and retains old keys for
        // historical reward claims.
        std::array<unsigned char, 32> flowmesh_ikm1{};
        flowmesh_ikm1.fill(0x71);
        std::array<unsigned char, 32> flowmesh_ikm2{};
        flowmesh_ikm2.fill(0x72);
        const auto flowmesh_key1{bls::SecretKey::FromIKM(flowmesh_ikm1)};
        const auto flowmesh_key2{bls::SecretKey::FromIKM(flowmesh_ikm2)};
        BOOST_REQUIRE(flowmesh_key1);
        BOOST_REQUIRE(flowmesh_key2);
        const auto flowmesh_pubkey1{flowmesh_key1->GetPublicKey().Compressed()};
        const auto flowmesh_pubkey2{flowmesh_key2->GetPublicKey().Compressed()};
        BOOST_REQUIRE(wallet->ImportFlowMeshBlsKey(*flowmesh_key1));
        BOOST_REQUIRE(wallet->ImportFlowMeshBlsKey(*flowmesh_key2));
        // Re-import is idempotent and cannot replace another key.
        BOOST_REQUIRE(wallet->ImportFlowMeshBlsKey(*flowmesh_key1));
        BOOST_CHECK_EQUAL(wallet->ListFlowMeshBlsPubkeys().size(), 2U);
        BOOST_CHECK(wallet->HasFlowMeshBlsKey(flowmesh_pubkey1));
        BOOST_CHECK(wallet->HasFlowMeshBlsKey(flowmesh_pubkey2));
        {
            const auto back{wallet->GetFlowMeshBlsKey(flowmesh_pubkey2)};
            BOOST_REQUIRE_MESSAGE(back, util::ErrorString(back).original);
            BOOST_CHECK(back->Bytes() == flowmesh_key2->Bytes());
        }
        // ENCRYPTION LIFECYCLE: EncryptWallet converts the plain record to
        // ciphertext in the same batch; a locked wallet refuses the key;
        // unlocking returns the identical scalar; re-import while encrypted
        // stores ciphertext only.
        BOOST_REQUIRE(wallet->EncryptWallet("qualification-pass"));
        BOOST_CHECK(wallet->IsLocked());
        BOOST_CHECK(!wallet->GetImportedFinalityBlsKey());
        BOOST_CHECK(!wallet->GetFlowMeshBlsKey(flowmesh_pubkey1));
        BOOST_CHECK(!wallet->GetFlowMeshAccountSecret());
        BOOST_REQUIRE(wallet->Unlock("qualification-pass"));
        {
            const auto account_secret{wallet->GetFlowMeshAccountSecret()};
            BOOST_REQUIRE(account_secret);
            BOOST_CHECK(account_secret->GetPubKey() == *flowmesh_account);
        }
        {
            const auto back{wallet->GetImportedFinalityBlsKey()};
            BOOST_REQUIRE_MESSAGE(back, util::ErrorString(back).original);
            BOOST_CHECK(back->Bytes() == independent->Bytes());
            BOOST_CHECK(back->GetPublicKey().Compressed() == independent->GetPublicKey().Compressed());
        }
        {
            const auto back1{wallet->GetFlowMeshBlsKey(flowmesh_pubkey1)};
            const auto back2{wallet->GetFlowMeshBlsKey(flowmesh_pubkey2)};
            BOOST_REQUIRE(back1);
            BOOST_REQUIRE(back2);
            BOOST_CHECK(back1->Bytes() == flowmesh_key1->Bytes());
            BOOST_CHECK(back2->Bytes() == flowmesh_key2->Bytes());
        }
        {
            std::array<unsigned char, 32> ikm2{};
            ikm2.fill(0x6B);
            const auto second{bls::SecretKey::FromIKM(ikm2)};
            BOOST_REQUIRE(second);
            const auto imported2{wallet->ImportFinalityBlsKey(*second)};
            BOOST_REQUIRE_MESSAGE(imported2, util::ErrorString(imported2).original);
            const auto back2{wallet->GetImportedFinalityBlsKey()};
            BOOST_REQUIRE(back2);
            BOOST_CHECK(back2->Bytes() == second->Bytes());
        }
        {
            std::array<unsigned char, 32> flowmesh_ikm3{};
            flowmesh_ikm3.fill(0x73);
            const auto flowmesh_key3{
                bls::SecretKey::FromIKM(flowmesh_ikm3)};
            BOOST_REQUIRE(flowmesh_key3);
            const auto imported3{
                wallet->ImportFlowMeshBlsKey(*flowmesh_key3)};
            BOOST_REQUIRE_MESSAGE(imported3,
                                  util::ErrorString(imported3).original);
            const auto back3{wallet->GetFlowMeshBlsKey(
                flowmesh_key3->GetPublicKey().Compressed())};
            BOOST_REQUIRE(back3);
            BOOST_CHECK(back3->Bytes() == flowmesh_key3->Bytes());
            BOOST_CHECK_EQUAL(wallet->ListFlowMeshBlsPubkeys().size(), 3U);
        }

        // Ownership and standardness through the carrier.
        BOOST_CHECK(wallet->IsMine(CTxOut{COIN, stake_script}));
        BOOST_CHECK(!wallet->IsMine(CTxOut{COIN, witness_owner_stake}));
        // FULL_SCRIPT is the conservative/pre-H B3A1 context. B3S1 still
        // unwraps because legacy Script drops its payload before executing
        // the owner suffix; era-gating this would strand ordinary funds.
        BOOST_CHECK(OutputScriptForWalletContext(
                        CTxOut{COIN, stake_script},
                        AssetSigningContext::FULL_SCRIPT) ==
                    GetScriptForDestination(owner));
        std::vector<std::vector<unsigned char>> solutions;
        BOOST_CHECK(Solver(stake_script, solutions) == TxoutType::PUBKEYHASH);
        BOOST_CHECK(Solver(witness_owner_stake, solutions) == TxoutType::NONSTANDARD);
        TxoutType type;
        BOOST_CHECK(IsStandard(stake_script, type));
        CTxDestination dest;
        BOOST_CHECK(ExtractDestination(stake_script, dest));
        BOOST_CHECK(dest == owner);

        // Unstake = an ordinary spend the wallet can sign (scriptCode is the full carrier).
        CMutableTransaction spend;
        spend.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
        spend.vout.emplace_back(COIN - 1000, GetScriptForDestination(owner));
        std::map<COutPoint, Coin> coins;
        coins[spend.vin[0].prevout] = Coin{CTxOut{COIN, stake_script}, /*nHeightIn=*/1, /*fCoinBaseIn=*/false};
        std::map<int, bilingual_str> input_errors;
        BOOST_CHECK(wallet->SignTransaction(spend, coins, SIGHASH_ALL, input_errors));
        BOOST_CHECK(input_errors.empty());
        BOOST_CHECK(VerifyScript(spend.vin[0].scriptSig, stake_script, nullptr, STANDARD_SCRIPT_VERIFY_FLAGS,
                                 MutableTransactionSignatureChecker(&spend, 0, COIN, MissingDataBehavior::FAIL)));
    }

    // A confirmed STAKE output of ours: owned, counted, but never auto-selected.
    CMutableTransaction stake_tx;
    stake_tx.vin.emplace_back(COutPoint{m_coinbase_txns[0]->GetHash(), 0});
    const CAmount in_value{m_coinbase_txns[0]->vout[0].nValue};
    stake_tx.vout.emplace_back(in_value - 10000, stake_script);
    {
        FillableSigningProvider keystore;
        BOOST_REQUIRE(keystore.AddKey(coinbaseKey));
        SignatureData sigdata;
        BOOST_REQUIRE(ProduceSignature(keystore, MutableTransactionSignatureCreator(stake_tx, 0, in_value, SIGHASH_ALL),
                                       m_coinbase_txns[0]->vout[0].scriptPubKey, sigdata));
        UpdateInput(stake_tx.vin[0], sigdata);
    }
    const CBlock stake_block{CreateAndProcessBlock({stake_tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()))};
    {
        LOCK(cs_main);
        BOOST_REQUIRE_EQUAL(m_node.chainman->ActiveChain().Height(), COINBASE_MATURITY + 1);
        BOOST_REQUIRE_EQUAL(m_node.chainman->ActiveChain().Tip()->GetBlockHash().GetHex(), stake_block.GetHash().GetHex());
        BOOST_REQUIRE_EQUAL(stake_block.vtx.size(), 2U);
        BOOST_REQUIRE_EQUAL(stake_block.vtx[1]->GetHash().GetHex(), stake_tx.GetHash().GetHex());
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(wallet->IsMine(*stake_block.vtx[1]));
    }
    {
        // The wallet is not subscribed to block notifications in this test, so
        // tell it the chain advanced (a rescan stops at the wallet's own
        // last-processed height) and rescan from genesis.
        LOCK2(wallet->cs_wallet, cs_main);
        wallet->SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
    }
    {
        WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        const uint256 genesis{WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Genesis()->GetBlockHash())};
        const CWallet::ScanResult result{wallet->ScanForWalletTransactions(genesis, /*start_height=*/0, /*max_height=*/{}, reserver, /*fUpdate=*/true, /*save_progress=*/false)};
        BOOST_REQUIRE(result.status == CWallet::ScanResult::SUCCESS);
    }
    {
        LOCK(wallet->cs_wallet);
        const COutPoint stake_outpoint{stake_tx.GetHash(), 0};
        BOOST_REQUIRE(wallet->GetWalletTx(stake_outpoint.hash) != nullptr);
        BOOST_CHECK(!wallet->IsSpent(stake_outpoint));
        bool listed{false};
        for (const COutput& coin : AvailableCoins(*wallet).All()) listed |= coin.outpoint == stake_outpoint;
        BOOST_CHECK(!listed);
    }
}

BOOST_AUTO_TEST_CASE(legacy_coinstake_maturity_depth_and_conflict)
{
    const auto make_coinstake = [](bool legacy_encoding) {
        CMutableTransaction tx;
        tx.m_legacy_encoding = legacy_encoding;
        tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
        tx.vout.emplace_back(0, CScript{});
        tx.vout.emplace_back(COIN, CScript() << OP_TRUE);
        return MakeTransactionRef(std::move(tx));
    };

    const uint256 block_hash{GetRandHash()};
    CWalletTx coinstake{make_coinstake(/*legacy_encoding=*/true),
                        TxStateConfirmed{block_hash, /*height=*/100, /*index=*/1}};

    LOCK(m_wallet.cs_wallet);
    BOOST_REQUIRE(coinstake.IsCoinStake());

    // At depth 1, 30 more blocks are required by the conservative wallet
    // maturity calculation.
    m_wallet.SetLastBlockProcessed(/*block_height=*/100, block_hash);
    BOOST_CHECK_EQUAL(m_wallet.GetTxDepthInMainChain(coinstake), 1);
    BOOST_CHECK_EQUAL(m_wallet.GetTxBlocksToMaturity(coinstake), legacy::COINBASE_MATURITY);

    // Depth 30 is still one block short; depth 31 is mature.
    m_wallet.SetLastBlockProcessed(/*block_height=*/129, block_hash);
    BOOST_CHECK_EQUAL(m_wallet.GetTxDepthInMainChain(coinstake), 30);
    BOOST_CHECK_EQUAL(m_wallet.GetTxBlocksToMaturity(coinstake), 1);
    m_wallet.SetLastBlockProcessed(/*block_height=*/130, block_hash);
    BOOST_CHECK_EQUAL(m_wallet.GetTxDepthInMainChain(coinstake), 31);
    BOOST_CHECK_EQUAL(m_wallet.GetTxBlocksToMaturity(coinstake), 0);

    // An inactive coinstake is maximally immature.
    coinstake.m_state = TxStateInactive{};
    BOOST_CHECK_EQUAL(m_wallet.GetTxDepthInMainChain(coinstake), 0);
    BOOST_CHECK_EQUAL(m_wallet.GetTxBlocksToMaturity(coinstake), legacy::COINBASE_MATURITY + 1);

    // A reorged coinstake has negative depth. It must not hit the coinbase
    // assertion, and its outputs remain maximally immature.
    coinstake.m_state = TxStateBlockConflicted{GetRandHash(), /*height=*/120};
    BOOST_CHECK_EQUAL(m_wallet.GetTxDepthInMainChain(coinstake), -11);
    BOOST_CHECK_EQUAL(m_wallet.GetTxBlocksToMaturity(coinstake), legacy::COINBASE_MATURITY + 1);

    // IsCoinStake() recognizes a transaction shape, so the wallet wrapper
    // must also require historical encoding before applying legacy maturity.
    CWalletTx modern_shaped{make_coinstake(/*legacy_encoding=*/false),
                            TxStateConfirmed{block_hash, /*height=*/100, /*index=*/1}};
    BOOST_REQUIRE(modern_shaped.tx->IsCoinStake());
    BOOST_CHECK(!modern_shaped.IsCoinStake());
    BOOST_CHECK_EQUAL(m_wallet.GetTxBlocksToMaturity(modern_shaped), 0);
}

BOOST_AUTO_TEST_CASE(optional_data_wallet_replacement_is_monotonic)
{
    CKey key;
    key.MakeNewKey(/*fCompressedIn=*/true);
    const CScript owned_script{
        GetScriptForDestination(PKHash{key.GetPubKey()})};
    {
        LOCK(m_wallet.cs_wallet);
        LegacyDataSPKM* legacy{m_wallet.GetOrCreateLegacyDataSPKM()};
        BOOST_REQUIRE(legacy != nullptr);
        BOOST_REQUIRE(legacy->LoadKey(key, key.GetPubKey()));
        m_wallet.CacheNewScriptPubKeys({owned_script}, legacy);
    }

    CMutableTransaction base;
    base.version = 2;
    base.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    base.vout.emplace_back(COIN, owned_script);

    CMutableTransaction mpa_only{base};
    mpa_only.mpa.push_back(CMpaRecord{1, 1, {0x01}});
    CMutableTransaction witness_only{base};
    witness_only.vin[0].scriptWitness.stack.push_back({0x02});
    CMutableTransaction both{mpa_only};
    both.vin[0].scriptWitness.stack.push_back({0x02});
    CMutableTransaction changed_both{both};
    changed_both.mpa[0].payload = {0x03};

    const Txid txid{base.GetHash()};
    LOCK(m_wallet.cs_wallet);
    BOOST_REQUIRE(m_wallet.AddToWallet(MakeTransactionRef(base),
                                       TxStateInactive{}));
    BOOST_REQUIRE(m_wallet.AddToWallet(MakeTransactionRef(mpa_only),
                                       TxStateInactive{}));
    BOOST_REQUIRE(m_wallet.GetWalletTx(txid)->tx->HasMpa());
    BOOST_CHECK(!m_wallet.GetWalletTx(txid)->tx->HasWitness());
    const auto enriched_txo{m_wallet.GetTXO(COutPoint{txid, 0})};
    BOOST_REQUIRE(enriched_txo.has_value());
    BOOST_CHECK(&enriched_txo->GetTxOut() ==
                &m_wallet.GetWalletTx(txid)->tx->vout[0]);

    // A witness-only variant would drop the retained MPA, so ignore it.
    BOOST_REQUIRE(m_wallet.AddToWallet(MakeTransactionRef(witness_only),
                                       TxStateInactive{}));
    BOOST_REQUIRE(m_wallet.GetWalletTx(txid)->tx->HasMpa());
    BOOST_CHECK(!m_wallet.GetWalletTx(txid)->tx->HasWitness());

    // Adding witness while preserving the exact MPA is strict enrichment.
    BOOST_REQUIRE(m_wallet.AddToWallet(MakeTransactionRef(both),
                                       TxStateInactive{}));
    BOOST_CHECK(m_wallet.GetWalletTx(txid)->tx->HasMpa());
    BOOST_CHECK(m_wallet.GetWalletTx(txid)->tx->HasWitness());

    // Neither a stripped variant nor a variant mutating retained MPA may win.
    BOOST_REQUIRE(m_wallet.AddToWallet(MakeTransactionRef(base),
                                       TxStateInactive{}));
    BOOST_REQUIRE(m_wallet.AddToWallet(MakeTransactionRef(changed_both),
                                       TxStateInactive{}));
    const CTransactionRef stored{m_wallet.GetWalletTx(txid)->tx};
    BOOST_CHECK(stored->HasMpa());
    BOOST_CHECK(stored->HasWitness());
    BOOST_CHECK(stored->mpa == CTransaction{both}.mpa);
    BOOST_CHECK(stored->vin[0].scriptWitness.stack ==
                CTransaction{both}.vin[0].scriptWitness.stack);
    const auto stored_txo{m_wallet.GetTXO(COutPoint{txid, 0})};
    BOOST_REQUIRE(stored_txo.has_value());
    BOOST_CHECK(&stored_txo->GetTxOut() == &stored->vout[0]);
}

BOOST_AUTO_TEST_CASE(commit_transaction_allows_external_keyless_input)
{
    CMutableTransaction transaction;
    const Txid external_parent{Txid::FromUint256(GetRandHash())};
    transaction.vin.emplace_back(COutPoint{external_parent, 0});
    transaction.vout.emplace_back(1, CScript{} << OP_TRUE);
    const CTransactionRef tx{MakeTransactionRef(std::move(transaction))};

    // A FlowMesh type-9 publication owns its native fee input but not the
    // keyless vault parent. Committing it must retain the transaction without
    // assuming every input parent belongs to the wallet.
    BOOST_CHECK_NO_THROW(m_wallet.CommitTransaction(
        tx, {{"b3", "external-keyless-input-test"}}, {}));
    LOCK(m_wallet.cs_wallet);
    BOOST_CHECK(m_wallet.mapWallet.contains(tx->GetHash()));
    BOOST_CHECK(!m_wallet.mapWallet.contains(external_parent));
}

BOOST_AUTO_TEST_CASE(legacy_imported_key_asset_owner_needs_post_h_provenance)
{
    const std::optional<int> legacy_final_height{
        Consensus::LegacyFinalHeight(Params().GetConsensus())};
    BOOST_REQUIRE(legacy_final_height.has_value());

    CKey key;
    key.MakeNewKey(/*fCompressedIn=*/true);
    const CScript owner{
        GetScriptForDestination(PKHash{key.GetPubKey()})};
    modern::AssetId asset;
    asset.begin()[0] = 0x42;
    const auto asset_output{modern::MakeAssetOwnerOutput(
        asset, /*amount=*/1, modern::PolicyType::FN, owner)};
    BOOST_REQUIRE(asset_output.has_value());

    {
        LOCK(m_wallet.cs_wallet);
        LegacyDataSPKM* legacy{m_wallet.GetOrCreateLegacyDataSPKM()};
        BOOST_REQUIRE(legacy != nullptr);
        BOOST_REQUIRE(legacy->LoadKey(key, key.GetPubKey()));
        m_wallet.CacheNewScriptPubKeys({owner}, legacy);

        // Context-free inspection cannot distinguish an old lookalike from a
        // modern asset. A trusted post-H Coin context resolves the ordinary
        // owner script and must work with imported-key wallets as well as
        // descriptor wallets.
        BOOST_CHECK(!m_wallet.IsMine(*asset_output));
        BOOST_CHECK(m_wallet.IsMine(
            *asset_output, AssetSigningContext::OWNER_SUFFIX));
    }

    CMutableTransaction funding;
    funding.version = 2;
    funding.vin.emplace_back(
        COutPoint{Txid::FromUint256(uint256::ONE), 0});
    funding.vout.push_back(*asset_output);
    const CTransaction funding_tx{funding};

    const auto spend_from = [&](const int coin_height) {
        CMutableTransaction spend;
        spend.version = 2;
        spend.vin.emplace_back(COutPoint{funding_tx.GetHash(), 0});
        spend.vout.push_back(*asset_output);
        std::map<COutPoint, Coin> coins;
        coins.emplace(spend.vin[0].prevout,
                      Coin{*asset_output, coin_height,
                           /*fCoinBaseIn=*/false});
        return std::pair{std::move(spend), std::move(coins)};
    };

    auto [pre_h_spend, pre_h_coins]{
        spend_from(*legacy_final_height)};
    std::map<int, bilingual_str> input_errors;
    BOOST_CHECK(!m_wallet.SignTransaction(
        pre_h_spend, pre_h_coins, SIGHASH_ALL, input_errors));
    BOOST_CHECK(pre_h_spend.vin[0].scriptSig.empty());

    auto [post_h_spend, post_h_coins]{
        spend_from(*legacy_final_height + 1)};
    input_errors.clear();
    BOOST_REQUIRE(m_wallet.SignTransaction(
        post_h_spend, post_h_coins, SIGHASH_ALL, input_errors));
    BOOST_CHECK(input_errors.empty());

    const CTransaction signed_tx{post_h_spend};
    ScriptError script_error{SCRIPT_ERR_UNKNOWN_ERROR};
    BOOST_CHECK(VerifyScript(
        signed_tx.vin[0].scriptSig, asset_output->scriptPubKey,
        &signed_tx.vin[0].scriptWitness, STANDARD_SCRIPT_VERIFY_FLAGS,
        TransactionSignatureChecker{
            &signed_tx, 0, asset_output->nValue,
            MissingDataBehavior::ASSERT_FAIL},
        &script_error, /*enable_asset_owner=*/true));

    // Fee estimation and preset-input funding must use the same trusted
    // provenance rule as signing. A wallet-known post-H carrier is solvable
    // through its owner suffix; byte-identical pre-H script semantics remain
    // full-script and therefore fail closed.
    CMutableTransaction pre_h_funding{funding};
    pre_h_funding.nLockTime = 1;
    const CTransaction pre_h_funding_tx{pre_h_funding};
    const uint256 block_hash{GetRandHash()};
    BOOST_REQUIRE(m_wallet.AddToWallet(
        MakeTransactionRef(funding_tx),
        TxStateConfirmed{block_hash, *legacy_final_height + 1, /*index=*/1}));
    BOOST_REQUIRE(m_wallet.AddToWallet(
        MakeTransactionRef(pre_h_funding_tx),
        TxStateConfirmed{block_hash, *legacy_final_height, /*index=*/2}));

    FastRandomContext rng;
    CoinSelectionParams selection_params{
        rng,
        /*change_output_size=*/34,
        /*change_spend_size=*/148,
        /*min_change_target=*/1'000,
        /*effective_feerate=*/CFeeRate{1'000},
        /*long_term_feerate=*/CFeeRate{1'000},
        /*discard_feerate=*/CFeeRate{1'000},
        /*tx_noinputs_size=*/0,
        /*avoid_partial=*/false,
    };

    LOCK(m_wallet.cs_wallet);
    m_wallet.SetLastBlockProcessed(*legacy_final_height + 10, block_hash);
    const COutPoint post_h_outpoint{funding_tx.GetHash(), 0};
    const COutPoint pre_h_outpoint{pre_h_funding_tx.GetHash(), 0};
    BOOST_REQUIRE(m_wallet.GetTXO(post_h_outpoint).has_value());
    BOOST_CHECK(!m_wallet.GetTXO(pre_h_outpoint).has_value());

    CCoinControl post_h_control;
    post_h_control.Select(post_h_outpoint);
    const auto post_h_selected{
        FetchSelectedInputs(m_wallet, post_h_control, selection_params)};
    BOOST_REQUIRE(post_h_selected);
    BOOST_CHECK_EQUAL(post_h_selected->Size(), 1U);

    CCoinControl pre_h_control;
    pre_h_control.Select(pre_h_outpoint).SetTxOut(*asset_output);
    BOOST_CHECK(!FetchSelectedInputs(
        m_wallet, pre_h_control, selection_params));
}

BOOST_AUTO_TEST_CASE(asset_parent_context_controls_input_trust_and_txo_refresh)
{
    const std::optional<int> legacy_final_height{
        Consensus::LegacyFinalHeight(Params().GetConsensus())};
    BOOST_REQUIRE(legacy_final_height.has_value());

    CKey key;
    key.MakeNewKey(/*fCompressedIn=*/true);
    const CScript owner{
        GetScriptForDestination(PKHash{key.GetPubKey()})};
    modern::AssetId asset;
    asset.begin()[0] = 0x43;
    const auto asset_output{modern::MakeAssetOwnerOutput(
        asset, /*amount=*/2, modern::PolicyType::OWNER, owner)};
    BOOST_REQUIRE(asset_output.has_value());
    const std::optional<modern::AssetId> fn_asset{
        modern::ConfiguredFnAssetId(Params().GetConsensus())};
    BOOST_REQUIRE(fn_asset.has_value());
    const auto fn_output{modern::MakeAssetOwnerOutput(
        *fn_asset, /*amount=*/1, modern::PolicyType::FN, owner)};
    BOOST_REQUIRE(fn_output.has_value());

    CMutableTransaction parent;
    parent.vin.emplace_back(
        COutPoint{Txid::FromUint256(uint256::ONE), 0});
    parent.vout.push_back(*asset_output);
    const CTransactionRef parent_ref{MakeTransactionRef(parent)};

    CMutableTransaction old_lookalike{parent};
    old_lookalike.nLockTime = 1;
    const CTransactionRef old_ref{MakeTransactionRef(old_lookalike)};

    CMutableTransaction child;
    child.vin.emplace_back(COutPoint{parent_ref->GetHash(), 0});
    child.vout.push_back(*asset_output);
    const CTransactionRef child_ref{MakeTransactionRef(child)};

    CMutableTransaction fn_genesis;
    fn_genesis.vin.resize(1);
    fn_genesis.vin[0].prevout.SetNull();
    fn_genesis.vin[0].scriptSig = CScript() << CScriptNum{1} << CScriptNum{1};
    fn_genesis.vout.push_back(*fn_output);
    const CTransactionRef fn_genesis_ref{MakeTransactionRef(fn_genesis)};

    const uint256 block_hash{GetRandHash()};
    LOCK(m_wallet.cs_wallet);
    LegacyDataSPKM* legacy{m_wallet.GetOrCreateLegacyDataSPKM()};
    BOOST_REQUIRE(legacy != nullptr);
    BOOST_REQUIRE(legacy->LoadKey(key, key.GetPubKey()));
    m_wallet.CacheNewScriptPubKeys({owner}, legacy);

    BOOST_REQUIRE(m_wallet.AddToWallet(
        parent_ref, TxStateConfirmed{block_hash,
                                     *legacy_final_height + 1,
                                     /*index=*/1}));
    BOOST_REQUIRE(m_wallet.AddToWallet(
        old_ref, TxStateConfirmed{block_hash,
                                  *legacy_final_height,
                                  /*index=*/2}));
    BOOST_REQUIRE(m_wallet.AddToWallet(
        fn_genesis_ref, TxStateConfirmed{block_hash,
                                         *legacy_final_height + 1,
                                         /*index=*/0}));
    m_wallet.SetLastBlockProcessed(*legacy_final_height + 10, block_hash);

    WalletContext interface_context;
    const std::shared_ptr<CWallet> wallet_alias{
        &m_wallet, [](CWallet*) {}};
    const std::unique_ptr<interfaces::Wallet> wallet_interface{
        interfaces::MakeWallet(interface_context, wallet_alias)};

    // This is the exact backend consumed by WalletModel's nonblocking asset
    // cache. The trusted post-H carrier is reported once; the byte-identical
    // pre-H lookalike is absent, so it cannot inflate the confirmed total.
    const std::vector<interfaces::WalletAssetBalance> asset_balances{
        wallet_interface->getAssetBalances()};
    BOOST_REQUIRE_EQUAL(asset_balances.size(), 2U);
    const auto colored_balance{std::ranges::find_if(
        asset_balances, [&](const auto& balance) {
            return balance.asset_id == asset;
        })};
    BOOST_REQUIRE(colored_balance != asset_balances.end());
    BOOST_CHECK_EQUAL(colored_balance->confirmed, 2);
    BOOST_CHECK_EQUAL(colored_balance->unconfirmed, 0);
    BOOST_CHECK_EQUAL(colored_balance->immature, 0);
    BOOST_CHECK_EQUAL(colored_balance->spendable, 2);
    BOOST_CHECK(!colored_balance->is_fn);

    const auto fn_balance{std::ranges::find_if(
        asset_balances, [&](const auto& balance) {
            return balance.asset_id == *fn_asset;
        })};
    BOOST_REQUIRE(fn_balance != asset_balances.end());
    BOOST_CHECK_EQUAL(fn_balance->confirmed, 1);
    BOOST_CHECK_EQUAL(fn_balance->unconfirmed, 0);
    BOOST_CHECK_EQUAL(fn_balance->immature, 1);
    BOOST_CHECK_EQUAL(fn_balance->spendable, 0);
    BOOST_CHECK(fn_balance->is_fn);

    // CWalletTx-aware reporting unwraps the owner only with trusted post-H
    // provenance. This drives both gettransaction.details and the Qt Activity
    // model; the byte-identical pre-H lookalike must remain unknown.
    const CWalletTx* parent_wtx{m_wallet.GetWalletTx(parent_ref->GetHash())};
    const CWalletTx* old_wtx{m_wallet.GetWalletTx(old_ref->GetHash())};
    BOOST_REQUIRE(parent_wtx != nullptr);
    BOOST_REQUIRE(old_wtx != nullptr);
    std::list<COutputEntry> received;
    std::list<COutputEntry> sent;
    CAmount fee{0};
    CachedTxGetAmounts(m_wallet, *parent_wtx, received, sent, fee,
                       /*include_change=*/false);
    BOOST_REQUIRE_EQUAL(received.size(), 1U);
    BOOST_CHECK(sent.empty());
    BOOST_CHECK(std::get_if<PKHash>(&received.front().destination) != nullptr);
    BOOST_CHECK_EQUAL(received.front().vout, 0);

    CachedTxGetAmounts(m_wallet, *old_wtx, received, sent, fee,
                       /*include_change=*/false);
    BOOST_CHECK(received.empty());
    BOOST_CHECK(sent.empty());

    const interfaces::WalletTx parent_report{
        wallet_interface->getWalletTx(parent_ref->GetHash())};
    BOOST_REQUIRE_EQUAL(parent_report.txout_is_mine.size(), 1U);
    BOOST_REQUIRE_EQUAL(parent_report.txout_address.size(), 1U);
    BOOST_REQUIRE_EQUAL(parent_report.txout_address_is_mine.size(), 1U);
    BOOST_CHECK(parent_report.txout_is_mine[0]);
    BOOST_CHECK(parent_report.txout_address_is_mine[0]);
    BOOST_CHECK(std::get_if<PKHash>(&parent_report.txout_address[0]) != nullptr);

    const interfaces::WalletTx old_report{
        wallet_interface->getWalletTx(old_ref->GetHash())};
    BOOST_REQUIRE_EQUAL(old_report.txout_is_mine.size(), 1U);
    BOOST_REQUIRE_EQUAL(old_report.txout_address.size(), 1U);
    BOOST_REQUIRE_EQUAL(old_report.txout_address_is_mine.size(), 1U);
    BOOST_CHECK(!old_report.txout_is_mine[0]);
    BOOST_CHECK(!old_report.txout_address_is_mine[0]);
    BOOST_CHECK(std::get_if<CNoDestination>(
                    &old_report.txout_address[0]) != nullptr);

    BOOST_REQUIRE(m_wallet.AddToWallet(child_ref, TxStateInMempool{}));

    const CTxDestination owner_destination{PKHash{key.GetPubKey()}};
    const std::map<CTxDestination, CAmount> address_balances{
        GetAddressBalances(m_wallet)};
    BOOST_CHECK(address_balances.contains(owner_destination));
    const std::set<std::set<CTxDestination>> address_groupings{
        GetAddressGroupings(m_wallet)};
    BOOST_CHECK(std::ranges::any_of(
        address_groupings, [&](const auto& group) {
            return group.contains(owner_destination);
        }));

    const CWalletTx* child_wtx{m_wallet.GetWalletTx(child_ref->GetHash())};
    BOOST_REQUIRE(child_wtx != nullptr);
    BOOST_CHECK(InputIsMine(m_wallet, child_ref->vin[0]));
    BOOST_CHECK(CachedTxIsTrusted(m_wallet, *child_wtx));
    BOOST_REQUIRE(m_wallet.GetTXO(
        COutPoint{child_ref->GetHash(), 0}).has_value());

    const CTxIn old_input{COutPoint{old_ref->GetHash(), 0}};
    BOOST_CHECK(!InputIsMine(m_wallet, old_input));

    // Losing mempool provenance removes owner-suffix semantics. Refresh must
    // delete the formerly owned cached output rather than leave it spendable
    // until restart.
    m_wallet.transactionRemovedFromMempool(
        child_ref, MemPoolRemovalReason::EXPIRY);
    BOOST_CHECK(!m_wallet.GetTXO(
        COutPoint{child_ref->GetHash(), 0}).has_value());
}

BOOST_AUTO_TEST_CASE(b3_witness_assets_remain_visible_but_not_spendable)
{
    const std::optional<int> legacy_final_height{
        Consensus::LegacyFinalHeight(Params().GetConsensus())};
    BOOST_REQUIRE(legacy_final_height.has_value());

    CKey key;
    key.MakeNewKey(/*fCompressedIn=*/true);
    const CScript witness_owner{
        GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()})};
    const CScript wrapped_owner{
        GetScriptForDestination(ScriptHash{witness_owner})};
    const CScript legacy_owner{
        GetScriptForDestination(PKHash{key.GetPubKey()})};

    modern::AssetId asset;
    asset.begin()[0] = 0x71;
    const auto direct_output{modern::MakeAssetOwnerOutput(
        asset, /*amount=*/2, modern::PolicyType::OWNER, witness_owner)};
    const auto wrapped_output{modern::MakeAssetOwnerOutput(
        asset, /*amount=*/3, modern::PolicyType::OWNER, wrapped_owner)};
    BOOST_REQUIRE(direct_output.has_value());
    BOOST_REQUIRE(wrapped_output.has_value());

    CMutableTransaction parent;
    parent.vin.emplace_back(
        COutPoint{Txid::FromUint256(uint256::ONE), 0});
    parent.vout.push_back(*direct_output);
    parent.vout.push_back(*wrapped_output);
    const CTransactionRef parent_ref{MakeTransactionRef(parent)};
    const uint256 block_hash{GetRandHash()};

    LOCK(m_wallet.cs_wallet);
    LegacyDataSPKM* legacy{m_wallet.GetOrCreateLegacyDataSPKM()};
    BOOST_REQUIRE(legacy != nullptr);
    BOOST_REQUIRE(legacy->LoadKey(key, key.GetPubKey()));
    BOOST_REQUIRE(legacy->LoadCScript(witness_owner));
    m_wallet.CacheNewScriptPubKeys(
        {legacy_owner, witness_owner, wrapped_owner}, legacy);

    BOOST_CHECK(WalletCanSpendScriptNow(m_wallet, legacy_owner));
    BOOST_CHECK(ScriptRequiresInactiveB3Witness(m_wallet, witness_owner));
    BOOST_CHECK(ScriptRequiresInactiveB3Witness(m_wallet, wrapped_owner));
    BOOST_CHECK(!WalletCanSpendScriptNow(m_wallet, witness_owner));
    BOOST_CHECK(!WalletCanSpendScriptNow(m_wallet, wrapped_owner));

    BOOST_REQUIRE(m_wallet.AddToWallet(
        parent_ref, TxStateConfirmed{block_hash,
                                     *legacy_final_height + 1,
                                     /*index=*/1}));
    m_wallet.SetLastBlockProcessed(*legacy_final_height + 10, block_hash);

    WalletContext interface_context;
    const std::shared_ptr<CWallet> wallet_alias{&m_wallet, [](CWallet*) {}};
    const std::unique_ptr<interfaces::Wallet> wallet_interface{
        interfaces::MakeWallet(interface_context, wallet_alias)};
    const std::vector<interfaces::WalletAssetBalance> balances{
        wallet_interface->getAssetBalances()};
    BOOST_REQUIRE_EQUAL(balances.size(), 1U);
    BOOST_CHECK(balances.front().asset_id == asset);
    BOOST_CHECK_EQUAL(balances.front().confirmed, 5);
    BOOST_CHECK_EQUAL(balances.front().spendable, 0);

    // Explicit asset coin selection fails with the same clear policy error.
    // The outputs remain in the wallet and in balance reporting for a future
    // release which intentionally activates SegWit.
    for (uint32_t vout{0}; vout < 2; ++vout) {
        CCoinControl coin_control;
        coin_control.Select(COutPoint{parent_ref->GetHash(), vout});
        FastRandomContext rng{/*fDeterministic=*/true};
        CoinSelectionParams params{rng};
        const auto selected{
            FetchSelectedInputs(m_wallet, coin_control, params)};
        BOOST_CHECK(!selected);
        BOOST_CHECK(util::ErrorString(selected).original.find(
                        "witness addresses are not active") != std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
