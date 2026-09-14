// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>
#include <clientversion.h>
#include <crypto/common.h>
#include <common/args.h>
#include <interfaces/chain.h>
#include <legacy/codec.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <uint256.h>
#include <univalue.h>
#include <wallet/sqlite.h>
#include <wallet/migrate.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <wallet/walletutil.h>

#include <boost/test/unit_test.hpp>
#include <sqlite3.h>
#include <cstdlib>
#include <fstream>

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

/** Generated BDB v9 fixture: outer metadata/root, main metadata/root.
 * Only key/data records are used. It contains no keys or holder data. Keep
 * this byte-level writer independent of the reader under test. */
std::vector<std::byte> BdbFixture(const std::vector<std::pair<SerializeData, SerializeData>>& records)
{
    constexpr uint32_t PAGE_SIZE{4096};
    std::vector<std::byte> bytes(4 * PAGE_SIZE);
    auto meta = [&](uint32_t page, uint32_t root) {
        auto* p = bytes.data() + page * PAGE_SIZE;
        WriteLE32(p + 4, 1); // reset LSN: no external recovery log required
        WriteLE32(p + 8, page);
        WriteLE32(p + 12, 0x00053162);
        WriteLE32(p + 16, 9);
        WriteLE32(p + 20, PAGE_SIZE);
        p[25] = std::byte{9}; // BTree metadata
        WriteLE32(p + 32, 3);
        WriteLE32(p + 48, 0x20); // subdatabase flag
        WriteLE32(p + 76, 2); // minimum keys per BTree page
        WriteLE32(p + 84, 0x20); // BDB's default padding character
        WriteLE32(p + 88, root);
    };
    auto leaf = [&](uint32_t page, const std::vector<SerializeData>& items) {
        auto* p = bytes.data() + page * PAGE_SIZE;
        WriteLE32(p + 4, 1);
        WriteLE32(p + 8, page);
        WriteLE16(p + 20, items.size());
        p[24] = std::byte{1};
        p[25] = std::byte{5}; // BTree leaf
        size_t offset{PAGE_SIZE};
        for (size_t i{0}; i < items.size(); ++i) {
            const size_t record_size{(3 + items[i].size() + 3) & ~size_t{3}};
            BOOST_REQUIRE(offset > record_size + 26 + 2 * items.size());
            offset -= record_size;
            WriteLE16(p + 26 + 2 * i, offset);
            WriteLE16(p + offset, items[i].size());
            p[offset + 2] = std::byte{1}; // key/data
            std::copy(items[i].begin(), items[i].end(), p + offset + 3);
        }
        WriteLE16(p + 22, offset);
    };
    meta(0, 1);
    leaf(1, {{std::byte{'m'}, std::byte{'a'}, std::byte{'i'}, std::byte{'n'}},
             {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{2}}});
    meta(2, 3);
    std::vector<SerializeData> items;
    for (const auto& [key, value] : records) {
        items.push_back(key);
        items.push_back(value);
    }
    leaf(3, items);
    return bytes;
}

void WriteGeneratedBdb(const fs::path& path, std::span<const std::byte> bytes)
{
    BOOST_REQUIRE(!fs::exists(path));
    AutoFile file{fsbridge::fopen(path, "wb")};
    BOOST_REQUIRE(!file.IsNull());
    file.write(bytes);
    BOOST_REQUIRE_EQUAL(file.fclose(), 0);
}

std::vector<std::byte> ReadGeneratedBdb(const fs::path& path)
{
    AutoFile file{fsbridge::fopen(path, "rb")};
    BOOST_REQUIRE(!file.IsNull());
    std::vector<std::byte> bytes(file.size());
    file.read(bytes);
    return bytes;
}

enum class MigrationFault { NONE, BEGIN, POPULATE, COMMIT, VERIFY, EXCEPTION };

class MigrationFaultBatch : public SQLiteBatch
{
    MigrationFault m_fault;
public:
    MigrationFaultBatch(SQLiteDatabase& database, MigrationFault fault) : SQLiteBatch(database), m_fault(fault) {}
    bool TxnBegin() override { return m_fault != MigrationFault::BEGIN && SQLiteBatch::TxnBegin(); }
    bool TxnCommit() override { return m_fault != MigrationFault::COMMIT && SQLiteBatch::TxnCommit(); }
    std::unique_ptr<DatabaseCursor> GetNewCursor() override
    {
        if (m_fault == MigrationFault::VERIFY) return nullptr;
        return SQLiteBatch::GetNewCursor();
    }
};

// Real SQLite storage/transactions; only the existing virtual DB/batch seam is
// faulted. No runtime fault option is exposed to wallet holders or RPC clients.
class MigrationFaultDatabase : public SQLiteDatabase
{
    MigrationFault m_fault;
public:
    MigrationFaultDatabase(const fs::path& path, MigrationFault fault) :
        SQLiteDatabase(path, path / "wallet.dat", DatabaseOptions{}), m_fault(fault)
    {
        if (fault == MigrationFault::POPULATE) {
            BOOST_REQUIRE_EQUAL(sqlite3_exec(m_db,
                "CREATE TRIGGER fail_second_insert BEFORE INSERT ON main "
                "WHEN (SELECT COUNT(*) FROM main)>0 BEGIN SELECT RAISE(ABORT,'injected write failure'); END",
                nullptr, nullptr, nullptr), SQLITE_OK);
        }
    }
    std::unique_ptr<DatabaseBatch> MakeBatch() override
    {
        if (m_fault == MigrationFault::EXCEPTION) throw std::runtime_error("injected replacement batch exception");
        return std::make_unique<MigrationFaultBatch>(*this, m_fault);
    }
};

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

BOOST_AUTO_TEST_CASE(b3_berkeley_reader_rejects_duplicate_live_records)
{
    const SerializeData first_key{std::byte{1}, std::byte{'a'}};
    const SerializeData second_key{std::byte{1}, std::byte{'b'}};
    const SerializeData first_value{std::byte{11}};
    const SerializeData second_value{std::byte{22}};
    const DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str error;

    const auto good_bytes{BdbFixture({{first_key, first_value}, {second_key, second_value}})};
    const fs::path good_path{m_path_root / "generated-valid-bdb.dat"};
    WriteGeneratedBdb(good_path, good_bytes);
    auto good{MakeBerkeleyRODatabase(good_path, options, status, error)};
    BOOST_REQUIRE(good);
    BOOST_REQUIRE_EQUAL(good->m_records.size(), 2U);
    BOOST_CHECK(good->m_records.at(first_key) == first_value);
    BOOST_CHECK(good->m_records.at(second_key) == second_value);
    good->Open();
    BOOST_REQUIRE_EQUAL(good->m_records.size(), 2U);

    // A wallet BDB is a unique-key database. Neither conflicting nor identical
    // duplicate live entries may be silently dropped by the migration reader.
    for (const auto& duplicate_value : {first_value, second_value}) {
        const fs::path bad_path{m_path_root / (duplicate_value == first_value ? "generated-identical-duplicate.dat" : "generated-conflicting-duplicate.dat")};
        const auto bad_bytes{BdbFixture({{first_key, first_value}, {first_key, duplicate_value}})};
        WriteGeneratedBdb(bad_path, bad_bytes);
        error = {};
        auto bad{MakeBerkeleyRODatabase(bad_path, options, status, error)};
        BOOST_CHECK(!bad);
        BOOST_CHECK(status == DatabaseStatus::FAILED_LOAD);
        BOOST_CHECK_EQUAL(error.original, "Duplicate live record key in BDB wallet");
        BOOST_CHECK(ReadGeneratedBdb(bad_path) == bad_bytes);
        BOOST_CHECK(ReadGeneratedBdb(good_path) == good_bytes);
    }
}

BOOST_AUTO_TEST_CASE(b3_migration_staged_replacement_failures_preserve_original)
{
    const CTransactionRef tx{MakeLegacyWalletTransaction(1'722'222'222, 17)};
    CWalletTx wtx{tx, TxStateInactive{}};
    wtx.nOrderPos = 0;
    wtx.nTimeReceived = 1'722'222'333;
    MockableData records;
    AddRawTxRecord(records, tx->GetHash(), HistoricalWalletTxValue(wtx, true));
    DataStream flags_key, flags_value;
    flags_key << DBKeys::FLAGS;
    flags_value << uint64_t{WALLET_FLAG_DESCRIPTORS};
    records.emplace(SerializeData(flags_key.begin(), flags_key.end()), SerializeData(flags_value.begin(), flags_value.end()));
    const std::vector<std::pair<SerializeData, SerializeData>> record_vector(records.begin(), records.end());
    const auto original{BdbFixture(record_vector)};
    const fs::path legacy_path{m_path_root / "generated-migration-original.dat"};
    WriteGeneratedBdb(legacy_path, original);

    for (const MigrationFault fault : {MigrationFault::BEGIN, MigrationFault::POPULATE,
             MigrationFault::COMMIT, MigrationFault::VERIFY, MigrationFault::EXCEPTION, MigrationFault::NONE}) {
        DatabaseOptions options;
        DatabaseStatus status;
        bilingual_str error;
        auto source{MakeBerkeleyRODatabase(legacy_path, options, status, error)};
        BOOST_REQUIRE(source);
        const fs::path replacement_path{m_path_root / fs::PathFromString("staged-fault-" + std::to_string(static_cast<int>(fault)))};
        {
            CWallet wallet{nullptr, "generated-migration", std::move(source)};
            LOCK(wallet.cs_wallet);
            auto replacement{std::make_unique<MigrationFaultDatabase>(replacement_path, fault)};
            const bool success{wallet.MigrateToSQLite(std::move(replacement), error)};
            BOOST_CHECK_EQUAL(success, fault == MigrationFault::NONE);
            BOOST_CHECK_EQUAL(wallet.GetDatabase().Format(), success ? "sqlite" : "bdb_ro");
            BOOST_CHECK(ReadGeneratedBdb(legacy_path) == original);
            if (!success) BOOST_CHECK(!error.empty());
        }

        // Close/reopen independently: failed begin/write/commit cannot publish
        // a partially committed destination. Verification failure may retain
        // a complete staged file, but it was never adopted or published.
        error = {};
        auto reopened{MakeSQLiteDatabase(replacement_path, options, status, error)};
        BOOST_REQUIRE_MESSAGE(reopened, error.original);
        auto batch{reopened->MakeBatch()};
        auto cursor{batch->GetNewCursor()};
        BOOST_REQUIRE(cursor);
        std::map<SerializeData, SerializeData> actual;
        DataStream key, value;
        DatabaseCursor::Status cursor_status;
        while ((cursor_status = cursor->Next(key, value)) == DatabaseCursor::Status::MORE) {
            actual.emplace(SerializeData(key.begin(), key.end()), SerializeData(value.begin(), value.end()));
        }
        BOOST_CHECK(cursor_status == DatabaseCursor::Status::DONE);
        const bool committed{fault == MigrationFault::NONE || fault == MigrationFault::VERIFY};
        BOOST_CHECK_EQUAL(actual.size(), committed ? records.size() : 0U);
        if (committed) {
            for (const auto& [record_key, record_value] : records) BOOST_CHECK(actual.at(record_key) == record_value);
        }
        cursor.reset();
        batch.reset();
        if (fault == MigrationFault::NONE) {
            CWallet reloaded{nullptr, "generated-reloaded", std::move(reopened)};
            BOOST_REQUIRE(WalletBatch{reloaded.GetDatabase()}.LoadWallet(&reloaded) == DBErrors::LOAD_OK);
            LOCK(reloaded.cs_wallet);
            const auto& loaded{reloaded.mapWallet.at(tx->GetHash())};
            BOOST_CHECK(loaded.GetHash() == tx->GetHash());
            BOOST_CHECK_EQUAL(loaded.tx->nTime, tx->nTime);
            BOOST_CHECK(loaded.tx->IsLegacyEncoded());
        }
        BOOST_CHECK(ReadGeneratedBdb(legacy_path) == original);
    }
    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str error;
    CWallet no_replacement{nullptr, "generated-null-replacement", MakeBerkeleyRODatabase(legacy_path, options, status, error)};
    LOCK(no_replacement.cs_wallet);
    BOOST_CHECK(!no_replacement.MigrateToSQLite(nullptr, error));
    BOOST_CHECK(ReadGeneratedBdb(legacy_path) == original);
    BOOST_CHECK_EQUAL(no_replacement.GetDatabase().Format(), "bdb_ro");
}

BOOST_FIXTURE_TEST_CASE(b3_migration_coordinator_failure_boundaries, RegTestingSetup)
{
    WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();
    const auto original{BdbFixture({})};
    const fs::path wallet_dir{GetWalletDir()};
    fs::create_directories(wallet_dir);
    const auto before_settings{context.chain->getRwSetting("wallet")};
    for (const std::string phase : {"before_backup", "backup_copied", "backup_verified",
             "replacement_created", "records_committed", "descriptors_committed",
             "before_publication", "original_retained", "after_publication", "cleanup_obstruction"}) {
        const std::string name{"generated-" + phase};
        const fs::path wallet_path{wallet_dir / fs::PathFromString(name)};
        const bool flat{phase == "original_retained"};
        if (!flat) fs::create_directory(wallet_path);
        const fs::path source{flat ? wallet_path : wallet_path / "wallet.dat"};
        WriteGeneratedBdb(source, original);
        DatabaseOptions options;
        DatabaseStatus status;
        bilingual_str error;
        auto database{MakeBerkeleyRODatabase(source, options, status, error)};
        BOOST_REQUIRE(database);
        auto wallet{std::make_shared<CWallet>(nullptr, name, std::move(database))};
        bool hit{false};
        fs::path backup, sentinel;
        auto outcome = MigrateLegacyToDescriptor(std::move(wallet), {}, context,
            [&](std::string_view event, const fs::path& path) {
                if (event == "backup_copied") backup = path;
                if (event == "before_cleanup" && phase == "cleanup_obstruction") {
                    sentinel = path / "unrelated.txt";
                    AutoFile file{fsbridge::fopen(sentinel, "wb")};
                    BOOST_REQUIRE(!file.IsNull());
                    file << std::string{"unrelated sentinel must survive"};
                    BOOST_REQUIRE_EQUAL(file.fclose(), 0);
                    return;
                }
                if ((phase == "cleanup_obstruction" && event == "before_publication") || event == phase) {
                    hit = true;
                    if (phase == "before_backup") {
                        // Real exclusive-copy refusal, not a mocked return.
                        fs::create_directory(path);
                    } else if (phase == "backup_copied") {
                        // Real post-copy byte verification must detect this.
                        AutoFile corrupt{fsbridge::fopen(path, "wb")};
                        BOOST_REQUIRE(!corrupt.IsNull());
                        corrupt << std::string{"injected truncated backup"};
                        BOOST_REQUIRE_EQUAL(corrupt.fclose(), 0);
                    } else {
                        throw std::runtime_error("injected boundary failure: " + phase);
                    }
                }
            });
        BOOST_REQUIRE(hit);
        BOOST_CHECK(!outcome);
        BOOST_CHECK(ReadGeneratedBdb(source) == original);
        BOOST_CHECK(GetWallets(context).empty());
        BOOST_CHECK_EQUAL(context.chain->getRwSetting("wallet").write(), before_settings.write());
        if (!backup.empty()) {
            BOOST_CHECK(fs::is_regular_file(backup));
            BOOST_CHECK_EQUAL(ReadGeneratedBdb(backup) == original, phase != "backup_copied");
        }
        if (!sentinel.empty()) BOOST_CHECK(fs::is_regular_file(sentinel));
        // Reopen the very same preserved BDB using the supported reader.
        auto reopened{MakeBerkeleyRODatabase(source, options, status, error)};
        BOOST_REQUIRE(reopened);
        BOOST_CHECK(reopened->m_records.empty());
    }
}

BOOST_FIXTURE_TEST_CASE(b3_migration_interruption_probe, RegTestingSetup)
{
    // Opt-in only in the test executable. The evidence runner supplies a fresh
    // generated testdatadir for each phase and captures intentional exit 86.
    const char* requested{std::getenv("B3_GENERATED_MIGRATION_INTERRUPT_PHASE")};
    if (!requested) return;
    const std::string phase{requested};
    BOOST_REQUIRE(phase == "before_publication" || phase == "original_retained" || phase == "after_publication");
    WalletContext context;
    context.args = m_node.args;
    context.chain = m_node.chain.get();
    const fs::path wallet_dir{GetWalletDir()};
    fs::create_directories(wallet_dir);
    const bool flat{phase == "original_retained"};
    const std::string name{"generated-interrupted"};
    const fs::path wallet_path{wallet_dir / name.c_str()};
    if (!flat) fs::create_directory(wallet_path);
    const fs::path source{flat ? wallet_path : wallet_path / "wallet.dat"};
    const auto original{BdbFixture({})};
    WriteGeneratedBdb(source, original);
    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str error;
    auto database{MakeBerkeleyRODatabase(source, options, status, error)};
    BOOST_REQUIRE(database);
    auto wallet{std::make_shared<CWallet>(nullptr, name, std::move(database))};
    fs::path backup;
    auto result = MigrateLegacyToDescriptor(std::move(wallet), {}, context,
        [&](std::string_view event, const fs::path& path) {
            if (event == "backup_verified") backup = path;
            if (event != phase) return;
            std::ofstream evidence{(m_path_root / "migration-interruption.txt").std_path()};
            evidence << phase << '\n' << fs::PathToString(source) << '\n'
                     << fs::PathToString(backup) << '\n' << fs::PathToString(path) << '\n';
            evidence.close();
            std::_Exit(86); // Deliberate process interruption, not a clean shutdown.
        });
    BOOST_FAIL("Requested interruption boundary was not reached");
}

BOOST_AUTO_TEST_CASE(b3_wallet_golden_history_preserves_encoding_before_hash)
{
    struct Vector {
        const char* hex;
        const char* txid;
        uint32_t n_time;
        bool coinbase;
        bool coinstake;
    };
    // Genesis and ordinary legacy literals are independently frozen in
    // legacy_identity_tests. The coinstake is a generated public codec vector,
    // not a claim that its script or reward is valid on a particular chain.
    const Vector vectors[]{
        {"010000001b735058010000000000000000000000000000000000000000000000000000000000000000ffffffff5300012a4c4e4368696e61206c61756e636865732047616f66656e2d3320537461656c6c69746520746f2067657420616363757261746520696d61676573206f66206561727468206f6e2031312d617567757374ffffffff0100000000000000000000000000",
         "4243fd570d4cb2e2930767f5bf18b2f65f1b7c4e16a392552d1efadeec00753d", 1481667355, true, false},
        {"010000005a5a5a5a01b300000000000000000000000000000000000000000000000000000000000000010000000151feffffff0187d6120000000000015107000000",
         "01ce3ea25423eb88dbb4525da9f53ef12add15ca744c80e26f98837ab3c77627", 0x5a5a5a5a, false, false},
        {"0100000001f153650101000000000000000000000000000000000000000000000000000000000000000500000000ffffffff020000000000000000000020a10700000000015100000000",
         "3d653f60664efbd410cc064b498dab1aa916ef0a9b7d8819eb8ea09f655f7340", 1700000001, false, true},
    };
    MockableData records;
    for (const auto& vector : vectors) {
        DataStream source{ParseHex<std::byte>(vector.hex)};
        CTransactionRef tx;
        source >> legacy::TX_LEGACY(tx);
        BOOST_REQUIRE(source.empty());
        BOOST_REQUIRE(tx->IsLegacyEncoded());
        // Immutable CTransaction construction must establish provenance before
        // caching the txid, not retrofit it after wallet/outpoint indexing.
        BOOST_CHECK_EQUAL(tx->GetHash().ToString(), vector.txid);
        BOOST_CHECK_EQUAL(tx->nTime, vector.n_time);
        CWalletTx wtx{tx, TxStateInactive{}};
        wtx.nOrderPos = records.size();
        wtx.nTimeReceived = 1800000000;
        wtx.nTimeSmart = 1800000001;
        AddRawTxRecord(records, tx->GetHash(), HistoricalWalletTxValue(wtx, true));
    }
    CMutableTransaction modern;
    DataStream modern_source{ParseHex<std::byte>("0100000001b300000000000000000000000000000000000000000000000000000000000000010000000151feffffff0187d6120000000000015107000000")};
    modern_source >> TX_MODERN(modern);
    BOOST_REQUIRE(!modern.IsLegacyEncoded());
    BOOST_REQUIRE_EQUAL(modern.nTime, 0U);
    CWalletTx modern_wtx{MakeTransactionRef(modern), TxStateInactive{}};
    modern_wtx.nOrderPos = records.size();
    DataStream modern_record;
    modern_record << modern_wtx;
    AddRawTxRecord(records, modern_wtx.GetHash(), modern_record);

    CWallet wallet{nullptr, "generated-mixed-era-history", CreateMockableWalletDatabase(std::move(records))};
    BOOST_REQUIRE(WalletBatch{wallet.GetDatabase()}.LoadWallet(&wallet) == DBErrors::LOAD_OK);
    LOCK(wallet.cs_wallet);
    BOOST_REQUIRE_EQUAL(wallet.mapWallet.size(), 4U);
    for (const auto& vector : vectors) {
        const auto parsed_hash{uint256::FromHex(vector.txid)};
        BOOST_REQUIRE(parsed_hash);
        const Txid txid{Txid::FromUint256(*parsed_hash)};
        const CWalletTx& loaded{wallet.mapWallet.at(txid)};
        BOOST_CHECK(loaded.tx->IsLegacyEncoded());
        BOOST_CHECK_EQUAL(loaded.tx->nTime, vector.n_time);
        BOOST_CHECK_EQUAL(loaded.nTimeReceived, 1800000000U);
        BOOST_CHECK_EQUAL(loaded.nTimeSmart, 1800000001U);
        BOOST_CHECK_EQUAL(loaded.IsCoinBase(), vector.coinbase);
        BOOST_CHECK_EQUAL(loaded.IsCoinStake(), vector.coinstake);
        DataStream reencoded;
        reencoded << legacy::TX_LEGACY(loaded.tx);
        BOOST_CHECK_EQUAL(HexStr(reencoded), vector.hex);
        // Outpoints retain the original transaction identity.
        BOOST_CHECK(COutPoint(loaded.GetHash(), 0) == COutPoint(txid, 0));
    }
    const CWalletTx& loaded_modern{wallet.mapWallet.at(modern_wtx.GetHash())};
    BOOST_CHECK(!loaded_modern.tx->IsLegacyEncoded());
    BOOST_CHECK_EQUAL(loaded_modern.tx->nTime, 0U);
    const auto legacy_hash{uint256::FromHex(vectors[1].txid)};
    BOOST_REQUIRE(legacy_hash);
    BOOST_CHECK(loaded_modern.GetHash() != Txid::FromUint256(*legacy_hash));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
