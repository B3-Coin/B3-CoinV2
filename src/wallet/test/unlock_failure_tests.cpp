// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <common/signmessage.h>
#include <rpc/request.h>
#include <rpc/util.h>
#include <scheduler.h>
#include <test/util/setup_common.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <boost/signals2/connection.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>

namespace wallet {
RPCHelpMan walletpassphrase();

namespace {
constexpr auto PASSPHRASE = "isolated unlock failure test passphrase";

// Test-only database faults exercise real keypool preparation, not a wallet
// mock that merely changes an isLocked flag.
class CommitFailureBatch : public MockableBatch
{
public:
    explicit CommitFailureBatch(MockableData& records) : MockableBatch(records, true) {}
    bool TxnCommit() override { throw std::runtime_error("injected keypool commit failure"); }
};

class UnlockFailureDatabase : public MockableDatabase
{
public:
    bool fail_commit{false};
    std::unique_ptr<DatabaseBatch> MakeBatch() override
    {
        if (fail_commit) return std::make_unique<CommitFailureBatch>(m_records);
        return MockableDatabase::MakeBatch();
    }
};

struct UnlockFailureSetup : BasicTestingSetup
{
    CScheduler scheduler;
    WalletContext context;
    std::shared_ptr<CWallet> wallet;
    PKHash address;

    UnlockFailureSetup() : BasicTestingSetup(ChainType::REGTEST, TestOpts{.setup_net = false})
    {
        // No chain mining, sockets, production database, or scheduler thread.
        wallet = std::make_shared<CWallet>(nullptr, "unlock-failure", std::make_unique<UnlockFailureDatabase>());
        {
            LOCK(wallet->cs_wallet);
            wallet->m_keypool_size = 1;
            wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            wallet->SetupDescriptorScriptPubKeyMans();
            const auto destination = wallet->GetNewDestination(OutputType::LEGACY, "offline-test");
            BOOST_REQUIRE(destination.has_value());
            address = std::get<PKHash>(*destination);
        }
        BOOST_REQUIRE(CanSign());
        BOOST_REQUIRE(wallet->EncryptWallet(PASSPHRASE));
        BOOST_REQUIRE(wallet->IsLocked());
        BOOST_REQUIRE(!CanSign());
        context.scheduler = &scheduler;
        WITH_LOCK(context.wallets_mutex, context.wallets.push_back(wallet));
    }

    UnlockFailureDatabase& Database()
    {
        return dynamic_cast<UnlockFailureDatabase&>(wallet->GetDatabase());
    }

    bool CanSign() const
    {
        std::string signature;
        return wallet->SignMessage("offline unlock regression; not a transaction", address, signature) == SigningResult::OK;
    }

    UniValue RpcUnlock(int timeout, const char* passphrase = PASSPHRASE)
    {
        JSONRPCRequest request;
        request.context = &context;
        request.strMethod = "walletpassphrase";
        request.params = UniValue{UniValue::VARR};
        request.params.push_back(passphrase);
        request.params.push_back(timeout);
        return walletpassphrase().HandleRequest(request);
    }

    size_t QueuedRelocks() const
    {
        std::chrono::steady_clock::time_point first, last;
        return scheduler.getQueueInfo(first, last);
    }

    void DrainRelocks()
    {
        scheduler.MockForward(std::chrono::seconds{180});
        scheduler.StopWhenDrained();
        scheduler.serviceQueue();
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(unlock_failure_tests, UnlockFailureSetup)

BOOST_AUTO_TEST_CASE(cache_failure_restores_locked_state)
{
    // Force the real cache upgrade's durable completion write to fail after
    // passphrase decryption and the raw master-key installation succeeded.
    wallet->UnsetWalletFlag(WALLET_FLAG_LAST_HARDENED_XPUB_CACHED);
    Database().m_pass = false;
    BOOST_CHECK_THROW(wallet->Unlock(PASSPHRASE), std::runtime_error);
    BOOST_CHECK(wallet->IsLocked());
    BOOST_CHECK(!CanSign());
    Database().m_pass = true;
    wallet->UnsetWalletFlag(WALLET_FLAG_LAST_HARDENED_XPUB_CACHED);
    BOOST_REQUIRE(wallet->Unlock(PASSPHRASE));
    BOOST_CHECK(CanSign());
}

BOOST_AUTO_TEST_CASE(cache_failure_preserves_preexisting_authorization)
{
    BOOST_REQUIRE(wallet->Unlock(PASSPHRASE));
    WITH_LOCK(wallet->cs_wallet, wallet->nRelockTime = 12345);
    wallet->UnsetWalletFlag(WALLET_FLAG_LAST_HARDENED_XPUB_CACHED);
    Database().m_pass = false;
    BOOST_CHECK_THROW(wallet->Unlock(PASSPHRASE), std::runtime_error);
    BOOST_CHECK(!wallet->IsLocked());
    BOOST_CHECK(CanSign());
    BOOST_CHECK_EQUAL(WITH_LOCK(wallet->cs_wallet, return wallet->nRelockTime), 12345);
    Database().m_pass = true;
    bool called{false};
    BOOST_CHECK(!wallet->Unlock("wrong passphrase", [&] { called = true; }));
    BOOST_CHECK(!called);
    BOOST_CHECK(CanSign());
}

BOOST_AUTO_TEST_CASE(callback_and_notification_failures_restore_prior_state)
{
    for (const bool initially_unlocked : {false, true}) {
        if (initially_unlocked) BOOST_REQUIRE(wallet->Unlock(PASSPHRASE));
        bool called{false};
        BOOST_CHECK_THROW(wallet->Unlock(PASSPHRASE, [&] {
            called = true;
            BOOST_CHECK(CanSign());
            throw std::runtime_error("injected authorization preparation failure");
        }), std::runtime_error);
        BOOST_CHECK(called);
        BOOST_CHECK_EQUAL(wallet->IsLocked(), !initially_unlocked);
        BOOST_CHECK_EQUAL(CanSign(), initially_unlocked);

        bool injected{false};
        boost::signals2::scoped_connection connection{wallet->NotifyStatusChanged.connect([&](CWallet*) {
            if (!injected && !wallet->IsLocked()) {
                injected = true;
                throw std::runtime_error("injected post-decryption notification failure");
            }
        })};
        BOOST_CHECK_THROW(wallet->Unlock(PASSPHRASE), std::runtime_error);
        BOOST_CHECK(injected);
        BOOST_CHECK_EQUAL(wallet->IsLocked(), !initially_unlocked);
        BOOST_CHECK_EQUAL(CanSign(), initially_unlocked);
    }
}

BOOST_AUTO_TEST_CASE(rpc_keypool_failures_restore_locked_state)
{
    // Both a false return from transaction begin and an exception during
    // commit occur after the core Unlock succeeds, before timer publication.
    Database().m_pass = false;
    BOOST_CHECK_THROW(RpcUnlock(60), UniValue);
    BOOST_CHECK(wallet->IsLocked());
    BOOST_CHECK(!CanSign());
    BOOST_CHECK_EQUAL(QueuedRelocks(), 0);
    BOOST_CHECK_EQUAL(WITH_LOCK(wallet->cs_wallet, return wallet->nRelockTime), 0);
    Database().m_pass = true;
    Database().fail_commit = true;
    BOOST_CHECK_THROW(RpcUnlock(60), std::runtime_error);
    BOOST_CHECK(wallet->IsLocked());
    BOOST_CHECK(!CanSign());
    BOOST_CHECK_EQUAL(QueuedRelocks(), 0);
    Database().fail_commit = false;
    BOOST_CHECK_NO_THROW(RpcUnlock(60));
    BOOST_CHECK(CanSign());
    BOOST_CHECK_EQUAL(QueuedRelocks(), 1);
}

BOOST_AUTO_TEST_CASE(rpc_failure_preserves_existing_timeout)
{
    BOOST_CHECK_NO_THROW(RpcUnlock(60));
    const auto original_deadline = WITH_LOCK(wallet->cs_wallet, return wallet->nRelockTime);
    BOOST_REQUIRE(CanSign());
    BOOST_REQUIRE_EQUAL(QueuedRelocks(), 1);
    Database().m_pass = false;
    BOOST_CHECK_THROW(RpcUnlock(120), UniValue);
    Database().m_pass = true;
    Database().fail_commit = true;
    BOOST_CHECK_THROW(RpcUnlock(120), std::runtime_error);
    Database().fail_commit = false;
    BOOST_CHECK_THROW(RpcUnlock(120, "wrong passphrase"), UniValue);
    BOOST_CHECK(CanSign());
    BOOST_CHECK_EQUAL(WITH_LOCK(wallet->cs_wallet, return wallet->nRelockTime), original_deadline);
    BOOST_CHECK_EQUAL(QueuedRelocks(), 1);
    // Execute the original callback without a sleep or network activity.
    DrainRelocks();
    BOOST_CHECK(wallet->IsLocked());
    BOOST_CHECK(!CanSign());
    BOOST_CHECK_EQUAL(WITH_LOCK(wallet->cs_wallet, return wallet->nRelockTime), 0);
}

BOOST_AUTO_TEST_CASE(rpc_missing_scheduler_and_zero_timeout)
{
    context.scheduler = nullptr;
    BOOST_CHECK_THROW(RpcUnlock(60), UniValue);
    BOOST_CHECK(wallet->IsLocked());
    BOOST_CHECK(!CanSign());
    context.scheduler = &scheduler;
    BOOST_CHECK_NO_THROW(RpcUnlock(0));
    BOOST_CHECK_EQUAL(QueuedRelocks(), 1);
    DrainRelocks();
    BOOST_CHECK(wallet->IsLocked());
    BOOST_CHECK(!CanSign());
    BOOST_CHECK_EQUAL(WITH_LOCK(wallet->cs_wallet, return wallet->nRelockTime), 0);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
