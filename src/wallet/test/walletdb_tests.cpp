// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>
#include <clientversion.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <uint256.h>
#include <wallet/sqlite.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(walletdb_tests, BasicTestingSetup)

namespace {

CTransactionRef MakeLegacyWalletTransaction(uint32_t n_time, uint32_t prevout_index)
{
    CMutableTransaction tx;
    tx.m_legacy_encoding = true;
    tx.version = 1;
    tx.nTime = n_time;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), prevout_index});
    tx.vout.emplace_back(500'000 + prevout_index, CScript{});
    return MakeTransactionRef(std::move(tx));
}

CTransactionRef MakeMpaWalletTransaction()
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 99});
    tx.vout.emplace_back(600'000, CScript{});
    CMpaRecord record;
    record.payload_type = 5;
    record.payload_version = 1;
    record.payload.assign(244, 0x42);
    tx.mpa = {std::move(record)};
    return MakeTransactionRef(std::move(tx));
}

/** Serialize the historical CWalletTx disk layout with a caller-selected
 * top-level transaction codec. A supporting transaction, when supplied, is
 * encoded exactly as old B3 wallets encoded vtxPrev's CMerkleTx entries. */
DataStream HistoricalWalletTxValue(const CWalletTx& wtx, bool keep_legacy_time,
                                   const CTransactionRef& supporting_tx = {})
{
    DataStream value;
    if (keep_legacy_time) {
        value << TX_LEGACY_B3(wtx.tx);
    } else {
        // This is the pre-fix wallet writer: nTime was irreversibly omitted.
        value << TX_WITH_WITNESS(wtx.tx);
    }

    const uint256 serialized_block_hash{TxStateSerializedBlockHash(wtx.m_state)};
    const int serialized_index{TxStateSerializedIndex(wtx.m_state)};
    value << serialized_block_hash << std::vector<uint256>{} << serialized_index;

    WriteCompactSize(value, supporting_tx ? 1 : 0);
    if (supporting_tx) {
        value << TX_LEGACY_B3(supporting_tx)
              << uint256::ZERO << std::vector<uint256>{} << int{-1};
    }

    mapValue_t map_value{wtx.mapValue};
    map_value["fromaccount"] = "";
    map_value["spent"] = "";
    if (wtx.nOrderPos != -1) map_value["n"] = util::ToString(wtx.nOrderPos);
    if (wtx.nTimeSmart != 0) map_value["timesmart"] = util::ToString(wtx.nTimeSmart);
    value << map_value << wtx.vOrderForm << uint32_t{0} << wtx.nTimeReceived << false << false;
    return value;
}

void AddRawTxRecord(MockableData& records, const Txid& txid, const DataStream& value)
{
    DataStream key;
    key << std::make_pair(DBKeys::TX, txid);
    records.emplace(SerializeData{key.begin(), key.end()}, SerializeData{value.begin(), value.end()});
}

} // namespace

BOOST_AUTO_TEST_CASE(walletdb_readkeyvalue)
{
    /**
     * When ReadKeyValue() reads from either a "key" or "wkey" it first reads the DataStream into a
     * CPrivKey or CWalletKey respectively and then reads a hash of the pubkey and privkey into a uint256.
     * Wallets from 0.8 or before do not store the pubkey/privkey hash, trying to read the hash from old
     * wallets throws an exception, for backwards compatibility this read is wrapped in a try block to
     * silently fail. The test here makes sure the type of exception thrown from DataStream::read()
     * matches the type we expect, otherwise we need to update the "key"/"wkey" exception type caught.
     */
    DataStream ssValue{};
    uint256 dummy;
    BOOST_CHECK_THROW(ssValue >> dummy, std::ios_base::failure);
}

BOOST_AUTO_TEST_CASE(b3_wallet_transactions_survive_sqlite_reopen)
{
    const CTransactionRef legacy_tx{MakeLegacyWalletTransaction(1'722'222'222, 1)};
    const CTransactionRef mpa_tx{MakeMpaWalletTransaction()};
    BOOST_REQUIRE(legacy_tx->IsLegacyEncoded());
    BOOST_REQUIRE(mpa_tx->HasMpa());

    CWalletTx legacy_wtx{legacy_tx, TxStateInactive{}};
    legacy_wtx.nOrderPos = 0;
    legacy_wtx.nTimeReceived = 1'722'222'333;
    legacy_wtx.nTimeSmart = 1'722'222'334;
    legacy_wtx.mapValue["b3-test"] = "legacy";

    CWalletTx mpa_wtx{mpa_tx, TxStateInactive{}};
    mpa_wtx.nOrderPos = 1;
    mpa_wtx.nTimeReceived = 1'722'222'444;
    mpa_wtx.mapValue["b3-test"] = "mpa";

    const fs::path database_path{m_path_root / "b3_wallet_tx_reopen"};
    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str error;
    auto database{MakeSQLiteDatabase(database_path, options, status, error)};
    BOOST_REQUIRE(database);
    {
        WalletBatch batch{*database};
        BOOST_REQUIRE(batch.WriteWalletFlags(WALLET_FLAG_DESCRIPTORS));
        BOOST_REQUIRE(batch.WriteTx(legacy_wtx));
        BOOST_REQUIRE(batch.WriteTx(mpa_wtx));
    }
    database->Close();
    database.reset();

    auto reopened{MakeSQLiteDatabase(database_path, options, status, error)};
    BOOST_REQUIRE(reopened);
    CWallet loaded{/*chain=*/nullptr, "b3-wallet-reopen", std::move(reopened)};
    const DBErrors load_result{WalletBatch{loaded.GetDatabase()}.LoadWallet(&loaded)};
    BOOST_REQUIRE(load_result == DBErrors::LOAD_OK);

    LOCK(loaded.cs_wallet);
    const CWalletTx& loaded_legacy{loaded.mapWallet.at(legacy_tx->GetHash())};
    BOOST_CHECK(loaded_legacy.tx->IsLegacyEncoded());
    BOOST_CHECK_EQUAL(loaded_legacy.tx->nTime, legacy_tx->nTime);
    BOOST_CHECK(loaded_legacy.GetHash() == legacy_tx->GetHash());
    BOOST_CHECK_EQUAL(loaded_legacy.nTimeReceived, legacy_wtx.nTimeReceived);
    BOOST_CHECK_EQUAL(loaded_legacy.mapValue.at("b3-test"), "legacy");

    const CWalletTx& loaded_mpa{loaded.mapWallet.at(mpa_tx->GetHash())};
    BOOST_CHECK(loaded_mpa.tx->HasMpa());
    BOOST_CHECK(loaded_mpa.tx->mpa == mpa_tx->mpa);
    BOOST_CHECK(loaded_mpa.tx->GetPtxid() == mpa_tx->GetPtxid());
    BOOST_CHECK_EQUAL(loaded_mpa.mapValue.at("b3-test"), "mpa");
}

BOOST_AUTO_TEST_CASE(b3_pre_fix_record_is_removed_before_rescan)
{
    const CTransactionRef legacy_tx{MakeLegacyWalletTransaction(1'733'333'333, 2)};
    CWalletTx legacy_wtx{legacy_tx, TxStateInactive{}};
    legacy_wtx.nOrderPos = 0;
    legacy_wtx.nTimeReceived = 1'733'333'444;

    // Reproduce the pre-fix record: its DB key is the historical txid, but its
    // value was written without nTime and therefore cannot reconstruct it.
    MockableData records;
    AddRawTxRecord(records, legacy_tx->GetHash(),
                   HistoricalWalletTxValue(legacy_wtx, /*keep_legacy_time=*/false));
    CWallet wallet{/*chain=*/nullptr, "b3-pre-fix-record", CreateMockableWalletDatabase(std::move(records))};
    const DBErrors first_load{WalletBatch{wallet.GetDatabase()}.LoadWallet(&wallet)};
    BOOST_REQUIRE(first_load == DBErrors::NEED_RESCAN);

    // A failed fill must not leave a partially decoded object under the real
    // txid. Otherwise AddToWallet sees it as existing and a rescan cannot heal
    // or rewrite the damaged record.
    {
        LOCK(wallet.cs_wallet);
        BOOST_REQUIRE(!wallet.mapWallet.contains(legacy_tx->GetHash()));
    }
    CWalletTx* recovered{wallet.AddToWallet(legacy_tx, TxStateInactive{})};
    BOOST_REQUIRE(recovered);
    BOOST_CHECK(recovered->tx->IsLegacyEncoded());
    BOOST_CHECK(recovered->GetHash() == legacy_tx->GetHash());

    // Prove that the rescan-style insertion overwrote the malformed value and
    // that the next process load no longer needs a rescan.
    auto reopened{DuplicateMockDatabase(wallet.GetDatabase())};
    CWallet reloaded{/*chain=*/nullptr, "b3-pre-fix-record-reloaded", std::move(reopened)};
    const DBErrors second_load{WalletBatch{reloaded.GetDatabase()}.LoadWallet(&reloaded)};
    BOOST_REQUIRE(second_load == DBErrors::LOAD_OK);
    LOCK(reloaded.cs_wallet);
    const CWalletTx& loaded{reloaded.mapWallet.at(legacy_tx->GetHash())};
    BOOST_CHECK(loaded.tx->IsLegacyEncoded());
    BOOST_CHECK_EQUAL(loaded.tx->nTime, legacy_tx->nTime);
}

BOOST_AUTO_TEST_CASE(b3_historical_wallet_record_with_legacy_vtxprev)
{
    const CTransactionRef supporting_tx{MakeLegacyWalletTransaction(1'700'000'001, 3)};
    const CTransactionRef wallet_tx{MakeLegacyWalletTransaction(1'700'000'002, 4)};
    CWalletTx historical_wtx{wallet_tx, TxStateInactive{}};
    historical_wtx.nOrderPos = 0;
    historical_wtx.nTimeReceived = 1'700'000'003;

    MockableData records;
    AddRawTxRecord(records, wallet_tx->GetHash(),
                   HistoricalWalletTxValue(historical_wtx, /*keep_legacy_time=*/true, supporting_tx));
    CWallet wallet{/*chain=*/nullptr, "b3-historical-vtxprev", CreateMockableWalletDatabase(std::move(records))};
    const DBErrors load_result{WalletBatch{wallet.GetDatabase()}.LoadWallet(&wallet)};
    BOOST_REQUIRE(load_result == DBErrors::LOAD_OK);

    LOCK(wallet.cs_wallet);
    const CWalletTx& loaded{wallet.mapWallet.at(wallet_tx->GetHash())};
    BOOST_CHECK(loaded.tx->IsLegacyEncoded());
    BOOST_CHECK_EQUAL(loaded.tx->nTime, wallet_tx->nTime);
    BOOST_CHECK(loaded.GetHash() == wallet_tx->GetHash());
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
