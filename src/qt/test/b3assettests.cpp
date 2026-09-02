// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/test/b3assettests.h>

#include <qt/b3assetmodel.h>
#include <qt/b3assetspage.h>

#include <interfaces/wallet.h>

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>

#include <string>
#include <utility>

namespace {

//! Deterministic sample source. Test-only: production views never see
//! fabricated data.
class TestAssetSource : public B3AssetSource
{
public:
    using B3AssetSource::B3AssetSource;

    QList<B3AssetRecord> assets() const override { return m_records; }
    bool coloredAssetsAvailable() const override { return true; }
    bool flowMeshAvailable() const override { return m_mesh; }

    void set(QList<B3AssetRecord> records)
    {
        m_records = std::move(records);
        Q_EMIT assetsChanged();
    }
    void setMeshAvailable(bool mesh) { m_mesh = mesh; }

private:
    QList<B3AssetRecord> m_records;
    bool m_mesh{false};
};

B3AssetRecord NativeRecord()
{
    B3AssetRecord record;
    record.asset_id = QStringLiteral("native");
    record.ticker = QStringLiteral("B3");
    record.display_name = QStringLiteral("B3");
    record.confirmed = 2'100'000'000'000'000LL;
    record.pending = 1;
    record.available = 2'100'000'000'000'000LL;
    record.decimals = 9;
    record.metadata_known = true;
    record.status = B3AssetRecord::Status::Native;
    return record;
}

} // namespace

void B3AssetTests::noWalletShowsEmptyState()
{
    B3AssetsPage page;
    QCOMPARE(page.model()->rowCount(), 0);

    bool no_wallet_note{false};
    for (const QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("No wallet")) && label->isVisibleTo(&page)) {
            no_wallet_note = true;
        }
    }
    QVERIFY(no_wallet_note);

    for (const char* name : {"assetSend", "assetReceive", "assetDeposit", "assetWithdraw"}) {
        auto* button = page.findChild<QPushButton*>(name);
        QVERIFY(button != nullptr);
        QVERIFY(!button->isEnabled());
    }
}

void B3AssetTests::amountFormattingIsIntegerExact()
{
    // 21 million coins at 8 decimals — integer-exact, thin-space grouped.
    const QString large = B3AssetTableModel::formatAmount(2'100'000'000'000'000LL, 8);
    QVERIFY(large.startsWith(QStringLiteral("21")));
    QVERIFY(large.endsWith(QStringLiteral(".00000000")));

    QCOMPARE(B3AssetTableModel::formatAmount(1, 8), QStringLiteral("0.00000001"));
    QCOMPARE(B3AssetTableModel::formatAmount(0, 8), QStringLiteral("0.00000000"));
    QCOMPARE(B3AssetTableModel::formatAmount(-150'000'000LL, 8), QStringLiteral("-1.50000000"));
    // Zero-decimal assets have no fractional part at all.
    QCOMPARE(B3AssetTableModel::formatAmount(42, 0), QStringLiteral("42"));
    // Different precisions round-trip the raw integer faithfully.
    QCOMPARE(B3AssetTableModel::formatAmount(123456, 3), QStringLiteral("123.456"));
}

void B3AssetTests::walletAssetRecordsExposeFnAndColoredAssets()
{
    const QString asset_id{
        QStringLiteral("2856d73456bec1845fc36234f247daa5816a1dfdd0bd8522c4a56db7389c4e76")};
    interfaces::WalletBalances native;
    native.balance = 42'000'000'000;
    native.immature_balance = 7'000'000'000;

    interfaces::WalletAssetBalance fn;
    fn.asset_id = uint256::FromHex(asset_id.toStdString()).value();
    fn.confirmed = 1;
    fn.spendable = 0;
    fn.immature = 1;
    fn.is_fn = true;

    interfaces::WalletAssetBalance colored;
    colored.asset_id = uint256::FromHex(std::string(64, '1')).value();
    colored.confirmed = 12'345;
    colored.unconfirmed = 5;
    colored.spendable = 12'000;

    interfaces::WalletAssetBalance bridge;
    bridge.asset_id = uint256::FromHex(std::string(64, '2')).value();
    bridge.confirmed = 1'250'000;
    bridge.spendable = 1'250'000;
    bridge.is_bridge = true;

    const QList<B3AssetRecord> records{
        B3NativeAssetSource::recordsForBalances(native, {fn, colored, bridge})};
    QCOMPARE(records.size(), 4);
    QCOMPARE(records.at(0).immature, 7'000'000'000);
    QCOMPARE(records.at(1).asset_id, asset_id);
    QCOMPARE(records.at(1).display_name, QStringLiteral("FN Coin"));
    QCOMPARE(records.at(1).ticker, QStringLiteral("FN"));
    QCOMPARE(records.at(1).confirmed, 1);
    QCOMPARE(records.at(1).available, 0);
    QCOMPARE(records.at(1).immature, 1);
    QCOMPARE(records.at(1).decimals, 0);
    QVERIFY(records.at(1).is_fn);

    QCOMPARE(records.at(2).asset_id, QString(64, QLatin1Char('1')));
    QCOMPARE(records.at(2).ticker, QStringLiteral("11111111"));
    QCOMPARE(records.at(2).confirmed, 12'345);
    QCOMPARE(records.at(2).pending, 5);
    QCOMPARE(records.at(2).available, 12'000);
    QCOMPARE(records.at(2).decimals, 0);
    QVERIFY(!records.at(2).metadata_known);
    QVERIFY(!records.at(2).is_fn);

    QCOMPARE(records.at(3).ticker, QStringLiteral("bUSD"));
    QCOMPARE(records.at(3).display_name, QStringLiteral("Bridged USD"));
    QCOMPARE(records.at(3).decimals, 6);
    QCOMPARE(records.at(3).available, 1'250'000);
    QVERIFY(records.at(3).metadata_known);
    QVERIFY(records.at(3).is_bridge);
}

void B3AssetTests::assetIdSearchSelectsOwnedAsset()
{
    B3AssetsPage page;
    TestAssetSource source;

    B3AssetRecord fn;
    fn.asset_id = QStringLiteral(
        "2856d73456bec1845fc36234f247daa5816a1dfdd0bd8522c4a56db7389c4e76");
    fn.ticker = QStringLiteral("FN");
    fn.display_name = QStringLiteral("FN Coin");
    fn.confirmed = 1;
    fn.immature = 1;
    fn.decimals = 0;
    fn.metadata_known = true;
    fn.is_fn = true;
    fn.status = B3AssetRecord::Status::Active;
    source.set({NativeRecord(), fn});
    page.setSource(&source);

    auto* search = page.findChild<QLineEdit*>("assetSearch");
    auto* list = page.findChild<QTableView*>("assetList");
    QVERIFY(search != nullptr);
    QVERIFY(list != nullptr);

    search->setText(fn.asset_id);
    QCOMPARE(list->model()->rowCount(), 1);
    QCOMPARE(list->currentIndex().data(B3AssetTableModel::AssetIdRole).toString(), fn.asset_id);
    QCOMPARE(page.findChild<QLabel*>("assetId")->text(), QStringLiteral("Asset ID: %1").arg(fn.asset_id));
    QVERIFY(page.findChild<QLabel*>("assetStatus")->text().contains(QStringLiteral("waiting for maturity")));

    page.setSource(nullptr);
}

void B3AssetTests::bridgeAssetDetailsUseBusdMetadata()
{
    B3AssetsPage page;
    TestAssetSource source;

    B3AssetRecord bridge;
    bridge.asset_id = QString(64, QLatin1Char('b'));
    bridge.ticker = QStringLiteral("bUSD");
    bridge.display_name = QStringLiteral("Bridged USD");
    bridge.confirmed = 1'250'000;
    bridge.available = 1'250'000;
    bridge.decimals = 6;
    bridge.metadata_known = true;
    bridge.is_bridge = true;
    bridge.status = B3AssetRecord::Status::Active;
    source.set({bridge});
    page.setSource(&source);

    QCOMPARE(page.model()->rowCount(), 1);
    QVERIFY(page.findChild<QLabel*>("assetStatus")->text().contains(
        QStringLiteral("Bridged USD")));

    page.setSource(nullptr);
}

void B3AssetTests::refreshPreservesSelectedAsset()
{
    B3AssetsPage page;
    TestAssetSource source;

    B3AssetRecord colored;
    colored.asset_id = QString(64, QLatin1Char('2'));
    colored.ticker = QStringLiteral("22222222");
    colored.display_name = QStringLiteral("Unknown asset");
    colored.available = 7;
    colored.decimals = 0;
    colored.status = B3AssetRecord::Status::Active;
    source.set({NativeRecord(), colored});
    page.setSource(&source);

    auto* list = page.findChild<QTableView*>("assetList");
    QVERIFY(list != nullptr);
    list->setCurrentIndex(list->model()->index(1, 0));
    QCOMPARE(list->currentIndex().data(B3AssetTableModel::AssetIdRole).toString(),
             colored.asset_id);

    // A wallet balance update resets the source model. The selected asset is
    // a stable user choice and must not silently jump back to native B3.
    colored.available = 8;
    source.set({NativeRecord(), colored});
    QCOMPARE(list->currentIndex().data(B3AssetTableModel::AssetIdRole).toString(),
             colored.asset_id);

    page.setSource(nullptr);
}

void B3AssetTests::nativeOnlySourceEnablesOnlySupportedActions()
{
    B3AssetsPage page;
    TestAssetSource source;
    source.set({NativeRecord()});
    page.setSource(&source);

    QCOMPARE(page.model()->rowCount(), 1);

    auto* send = page.findChild<QPushButton*>("assetSend");
    auto* receive = page.findChild<QPushButton*>("assetReceive");
    auto* deposit = page.findChild<QPushButton*>("assetDeposit");
    auto* withdraw = page.findChild<QPushButton*>("assetWithdraw");
    QVERIFY(send->isEnabled());
    QVERIFY(receive->isEnabled());
    QVERIFY(!deposit->isEnabled());
    QVERIFY(!withdraw->isEnabled());

    bool mesh_note{false};
    for (const QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("not available on this page")) &&
            label->isVisibleTo(&page)) {
            mesh_note = true;
        }
    }
    QVERIFY(mesh_note);

    // Data availability is not an approved deposit/withdraw action path.
    // Even a source advertising FlowMesh data must not make disconnected
    // controls look executable.
    source.setMeshAvailable(true);
    source.set({NativeRecord()});
    QVERIFY(!deposit->isEnabled());
    QVERIFY(!withdraw->isEnabled());

    // FlowMesh/reserved balances are "Not available", never a fake zero.
    int not_available{0};
    for (const QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("Not available")) ++not_available;
    }
    QCOMPARE(not_available, 2);

    QSignalSpy send_spy(&page, &B3AssetsPage::sendRequested);
    send->click();
    QCOMPARE(send_spy.count(), 1);

    page.setSource(nullptr);
}

void B3AssetTests::unknownMetadataAndLongNamesRenderSafely()
{
    B3AssetsPage page;
    TestAssetSource source;

    B3AssetRecord unknown;
    unknown.asset_id = QStringLiteral("asset:deadbeef");
    unknown.ticker = QStringLiteral("????");
    unknown.display_name = QString(400, QLatin1Char('X'));
    unknown.metadata_known = false;
    unknown.status = B3AssetRecord::Status::Unavailable;
    unknown.decimals = 0;
    source.set({unknown});
    page.setSource(&source);

    const QModelIndex name_index = page.model()->index(0, B3AssetTableModel::Name);
    QCOMPARE(name_index.data().toString(), QStringLiteral("Unknown asset"));

    // Unsupported asset: nothing actionable.
    QVERIFY(!page.findChild<QPushButton*>("assetSend")->isEnabled());
    QVERIFY(!page.findChild<QPushButton*>("assetReceive")->isEnabled());

    page.setSource(nullptr);
}

void B3AssetTests::insertionRemovalAndResetUpdateModel()
{
    B3AssetsPage page;
    TestAssetSource source;
    page.setSource(&source);
    QCOMPARE(page.model()->rowCount(), 0);

    QSignalSpy reset_spy(page.model(), &QAbstractItemModel::modelReset);

    source.set({NativeRecord()});
    QCOMPARE(page.model()->rowCount(), 1);

    B3AssetRecord second = NativeRecord();
    second.asset_id = QStringLiteral("asset:test");
    second.ticker = QStringLiteral("TST");
    second.display_name = QStringLiteral("Test Asset");
    second.status = B3AssetRecord::Status::Active;
    source.set({NativeRecord(), second});
    QCOMPARE(page.model()->rowCount(), 2);

    source.set({second});
    QCOMPARE(page.model()->rowCount(), 1);
    QCOMPARE(page.model()->recordAt(0).ticker, QStringLiteral("TST"));

    source.set({});
    QCOMPARE(page.model()->rowCount(), 0);
    QVERIFY(reset_spy.count() >= 4);

    page.setSource(nullptr);
}

void B3AssetTests::sourceDestructionDetachesModel()
{
    B3AssetsPage page;
    auto source = std::make_unique<TestAssetSource>();
    source->set({NativeRecord()});
    page.setSource(source.get());
    QCOMPARE(page.model()->rowCount(), 1);

    // Destroying the source must leave the model empty and detached, not
    // dangling.
    source.reset();
    QCOMPARE(page.model()->rowCount(), 0);
    QVERIFY(page.model()->source() == nullptr);
}
