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
    record.precision_known = true;
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
        QVERIFY(button->toolTip().contains(QStringLiteral("wallet")));
        QCOMPARE(button->accessibleDescription(), button->toolTip());
    }
    auto* reason{page.findChild<QLabel*>("assetActionReason")};
    QVERIFY(reason);
    QVERIFY(reason->isVisibleTo(&page));
    QCOMPARE(reason->textFormat(), Qt::PlainText);
}

void B3AssetTests::disabledActionReasonsFollowSelection()
{
    B3AssetsPage page;
    TestAssetSource source;
    B3AssetRecord asset;
    asset.asset_id = QString(64, QLatin1Char('6'));
    asset.status = B3AssetRecord::Status::Active;
    asset.confirmed = 100;
    source.set({asset});
    page.setSource(&source); // A display source is not a captured signing wallet.
    auto* send{page.findChild<QPushButton*>("assetSend")};
    auto* receive{page.findChild<QPushButton*>("assetReceive")};
    auto* reason{page.findChild<QLabel*>("assetActionReason")};
    QVERIFY(!send->isEnabled());
    QVERIFY(!receive->isEnabled());
    QVERIFY(reason->isVisibleTo(&page));
    QVERIFY(reason->text().contains(QStringLiteral("Send unavailable:")));
    QVERIFY(reason->text().contains(QStringLiteral("Receive unavailable:")));
    QVERIFY(send->toolTip().contains(QStringLiteral("loaded wallet")));

    source.set({NativeRecord()});
    QVERIFY(send->isEnabled());
    QVERIFY(receive->isEnabled());
    QVERIFY(!reason->isVisibleTo(&page));
    QVERIFY(!send->toolTip().contains(QStringLiteral("loaded wallet")));
    QVERIFY(!receive->toolTip().contains(QStringLiteral("loaded wallet")));

    source.set({});
    QVERIFY(!send->isEnabled());
    QVERIFY(!receive->isEnabled());
    QVERIFY(reason->isVisibleTo(&page));
    QVERIFY(reason->text().contains(QStringLiteral("Select an asset")));
    QCOMPARE(send->toolTip(), reason->text());
    QCOMPARE(receive->accessibleDescription(), reason->text());
    page.setSource(nullptr);
}

void B3AssetTests::relockWarningSurvivesRefreshDetachAndBlocksActions()
{
    B3AssetsPage page;
    TestAssetSource source;
    source.set({NativeRecord()});
    page.setSource(&source);
    auto* send{page.findChild<QPushButton*>("assetSend")};
    auto* receive{page.findChild<QPushButton*>("assetReceive")};
    auto* reason{page.findChild<QLabel*>("assetActionReason")};
    auto* fn_panel{page.findChild<QWidget*>("fnOperatorPanel")};
    QVERIFY(send && receive && reason && fn_panel);
    QVERIFY(send->isEnabled());
    QVERIFY(receive->isEnabled());
    QSignalSpy sent(&page, &B3AssetsPage::sendRequested);
    QSignalSpy received(&page, &B3AssetsPage::receiveRequested);

    const QString warning{QStringLiteral("SECURITY: spending relock failed for captured old wallet <test>.")};
    QVERIFY(QMetaObject::invokeMethod(&page, "showSecurityWarning", Qt::DirectConnection,
                                     Q_ARG(QString, warning)));
    QVERIFY(!send->isEnabled());
    QVERIFY(!receive->isEnabled());
    QVERIFY(!fn_panel->isEnabled());
    QCOMPARE(reason->textFormat(), Qt::PlainText);
    QVERIFY(reason->text().contains(warning));
    QVERIFY(QMetaObject::invokeMethod(&page, "sendSelectedAsset", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(&page, "receiveSelectedAsset", Qt::DirectConnection));
    QCOMPARE(sent.count(), 0);
    QCOMPARE(received.count(), 0);

    source.set({NativeRecord()}); // A balance/model refresh cannot clear it.
    QVERIFY(reason->text().contains(warning));
    page.setSource(nullptr);
    QCOMPARE(reason->text(), warning);
    QVERIFY(reason->isVisibleTo(&page));
    page.setSource(&source); // Nor can switching away and back to a wallet.
    QVERIFY(reason->text().contains(warning));
    QVERIFY(!send->isEnabled());
    QVERIFY(!receive->isEnabled());
    QVERIFY(!fn_panel->isEnabled());
    page.setSource(nullptr);
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
    QCOMPARE(B3AssetTableModel::formatAmount(1, 18), QStringLiteral("0.000000000000000001"));
}

void B3AssetTests::viewOnlyFnDataNeverEnablesOperatorActions()
{
    B3AssetsPage page;
    TestAssetSource source;
    B3AssetRecord fn;
    fn.asset_id = QString(64, QLatin1Char('6'));
    fn.status = B3AssetRecord::Status::Active;
    fn.confirmed = fn.available = 1;
    fn.is_fn = true;
    source.setMeshAvailable(true);
    source.set({NativeRecord(), fn});
    page.setSource(&source);
    auto* panel{page.findChild<QWidget*>("fnOperatorPanel")};
    QVERIFY(panel);
    QVERIFY(panel->isVisibleTo(&page));
    for (const char* name : {"fnBindKey", "fnArmKeys", "fnDisarmKeys", "fnRefresh"}) {
        auto* action{panel->findChild<QPushButton*>(name)};
        QVERIFY(action);
        QVERIFY(!action->isEnabled());
    }
    QVERIFY(page.findChild<QLabel*>("fnOperatorWallet")->text().contains(QStringLiteral("No wallet")));
    QVERIFY(!page.findChild<QPushButton*>("assetDeposit")->isEnabled());
    QVERIFY(!page.findChild<QPushButton*>("assetWithdraw")->isEnabled());
    page.setSource(nullptr);
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
    QVERIFY(!records.at(2).precision_known);
    QVERIFY(!records.at(2).is_fn);

    QCOMPARE(records.at(3).ticker, QStringLiteral("bUSD"));
    QCOMPARE(records.at(3).display_name, QStringLiteral("Bridged USD"));
    QCOMPARE(records.at(3).decimals, 6);
    QCOMPARE(records.at(3).available, 1'250'000);
    QVERIFY(records.at(3).metadata_known);
    QVERIFY(records.at(3).is_bridge);
}

void B3AssetTests::registeredAssetUsesVerifiedPrecision()
{
    interfaces::WalletAssetBalance balance;
    balance.asset_id = uint256::FromHex("43d4555d04fdb78726381db4e8340c6f59e5761f2945d634aef0a5d4a3c3a299").value();
    balance.confirmed = 1'000'000'000'000;
    balance.spendable = balance.confirmed;
    balance.display_name = "Test USD";
    balance.ticker = "tUSD";
    balance.decimals = 6;
    balance.metadata_source = "bundled-registry";
    balance.is_test_asset = true;
    const auto records{B3NativeAssetSource::recordsForBalances({}, {balance})};
    QCOMPARE(records.size(), 2);
    const B3AssetRecord& asset{records.at(1)};
    QVERIFY(asset.metadata_known);
    QVERIFY(asset.precision_known);
    QVERIFY(asset.is_test_asset);
    QCOMPARE(asset.display_name, QStringLiteral("Test USD"));
    QCOMPARE(asset.ticker, QStringLiteral("tUSD"));
    QCOMPARE(asset.decimals, 6);
    QCOMPARE(asset.available, 1'000'000'000'000);

    B3AssetsPage page;
    TestAssetSource source;
    source.set({asset});
    page.setSource(&source);
    QCOMPARE(page.model()->index(0, B3AssetTableModel::Name).data().toString(), QStringLiteral("Test USD"));
    QCOMPARE(page.model()->index(0, B3AssetTableModel::Available).data().toString(),
             QStringLiteral("1\u2009000\u2009000.000000"));
    QVERIFY(page.findChild<QLabel*>("assetStatus")->text().contains(QStringLiteral("Unbacked test asset")));
    QCOMPARE(page.findChild<QLabel*>("assetId")->text(), QStringLiteral("Asset ID: %1").arg(asset.asset_id));
    page.findChild<QLineEdit*>("assetSearch")->setText(QStringLiteral("tUSD"));
    QCOMPARE(page.findChild<QTableView*>("assetList")->model()->rowCount(), 1);
    // Display metadata must not turn disconnected trading controls on.
    QVERIFY(!page.findChild<QPushButton*>("assetDeposit")->isEnabled());
    QVERIFY(!page.findChild<QPushButton*>("assetSend")->isEnabled());
    const QString preview{qEnvironmentVariable("B3_ASSET_PREVIEW_PNG")};
    if (!preview.isEmpty()) {
        page.resize(1200, 850);
        page.show();
        QCoreApplication::processEvents();
        QVERIFY(page.grab().save(preview));
    }
    page.setSource(nullptr);
}

void B3AssetTests::precisionWithoutNameRemainsDistinctFromUnknown()
{
    interfaces::WalletAssetBalance balance;
    balance.asset_id = uint256::FromHex(std::string(64, '3')).value();
    balance.spendable = 12'345;
    balance.decimals = 6;
    balance.metadata_source = "wallet-issuance";
    auto asset{B3NativeAssetSource::recordsForBalances({}, {balance}).at(1)};
    QVERIFY(asset.precision_known);
    QVERIFY(!asset.metadata_known);
    QCOMPARE(B3AssetTableModel::assetName(asset), QStringLiteral("Unnamed asset"));
    QCOMPARE(B3AssetTableModel::formatAmount(asset.available, asset.decimals), QStringLiteral("0.012345"));

    balance.decimals = 0;
    asset = B3NativeAssetSource::recordsForBalances({}, {balance}).at(1);
    QVERIFY(asset.precision_known);
    QCOMPARE(asset.decimals, 0);
    balance.decimals.reset();
    asset = B3NativeAssetSource::recordsForBalances({}, {balance}).at(1);
    QVERIFY(!asset.precision_known);
    QCOMPARE(B3AssetTableModel::assetName(asset), QStringLiteral("Unknown asset"));

    // Invalid precision cannot enable a plausible-looking label or scale.
    balance.display_name = "False label";
    balance.ticker = "FALSE";
    for (const int bad : {-1, 19}) {
        balance.decimals = bad;
        asset = B3NativeAssetSource::recordsForBalances({}, {balance}).at(1);
        QVERIFY(!asset.precision_known);
        QVERIFY(!asset.metadata_known);
        QCOMPARE(asset.decimals, 0);
    }
}

void B3AssetTests::metadataRefreshUpdatesDisplayWithoutChangingBalances()
{
    interfaces::WalletAssetBalance balance;
    balance.asset_id = uint256::FromHex(std::string(64, '4')).value();
    balance.spendable = 2'500'000;
    B3AssetsPage page;
    TestAssetSource source;
    source.set({B3NativeAssetSource::recordsForBalances({}, {balance}).at(1)});
    page.setSource(&source);
    QCOMPARE(page.model()->index(0, B3AssetTableModel::Name).data().toString(), QStringLiteral("Unknown asset"));
    QSignalSpy reset_spy(page.model(), &QAbstractItemModel::modelReset);
    balance.display_name = "Example Asset";
    balance.ticker = "EXAMPLE";
    balance.decimals = 6;
    balance.metadata_source = "local-registry";
    source.set({B3NativeAssetSource::recordsForBalances({}, {balance}).at(1)});
    QCOMPARE(reset_spy.count(), 1);
    QCOMPARE(page.model()->index(0, B3AssetTableModel::Name).data().toString(), QStringLiteral("Example Asset"));
    QCOMPARE(page.model()->index(0, B3AssetTableModel::Available).data().toString(), QStringLiteral("2.500000"));
    QCOMPARE(page.model()->recordAt(0).available, balance.spendable);
    QVERIFY(page.findChild<QLabel*>("assetStatus")->text().contains(QStringLiteral("Local asset label")));
    QCOMPARE(page.findChild<QTableView*>("assetList")->currentIndex().data(B3AssetTableModel::AssetIdRole).toString(),
             QString::fromStdString(balance.asset_id.GetHex()));
    page.setSource(nullptr);
}

void B3AssetTests::assetLabelsRenderAsPlainText()
{
    B3AssetRecord asset{NativeRecord()};
    asset.asset_id = QString(64, QLatin1Char('5'));
    asset.status = B3AssetRecord::Status::Active;
    asset.display_name = QStringLiteral("<b>Untrusted label</b>");
    B3AssetsPage page;
    TestAssetSource source;
    source.set({asset});
    page.setSource(&source);
    const QLabel* label{page.findChild<QLabel*>("assetName")};
    QCOMPARE(label->textFormat(), Qt::PlainText);
    QVERIFY(label->toolTip().contains(QStringLiteral("&lt;b&gt;")));
    QVERIFY(!label->toolTip().contains(QStringLiteral("<b>")));
    QVERIFY(page.model()->index(0, 0).data(Qt::ToolTipRole).toString().contains(QStringLiteral("&lt;b&gt;")));
    page.setSource(nullptr);
}

void B3AssetTests::assetSendAllowsUnlockButRejectsWatchOnlyAndImmature()
{
    B3AssetRecord asset;
    asset.status = B3AssetRecord::Status::Active;
    asset.asset_id = QString(64, QLatin1Char('6'));
    asset.confirmed = 1'000'000;
    // Spending-locked wallets report available=0. Let them open the form and
    // request the normal spending unlock; never treat cached available as a
    // substitute for the backend's signing checks.
    asset.available = 0;
    QVERIFY(B3AssetsPage::canSendAsset(asset, true));
    QVERIFY(B3AssetsPage::sendAssetDisabledReason(asset, true).isEmpty());
    QVERIFY(!B3AssetsPage::canSendAsset(asset, false));
    QVERIFY(B3AssetsPage::sendAssetDisabledReason(asset, false).contains(QStringLiteral("watch-only")));
    asset.immature = asset.confirmed;
    QVERIFY(!B3AssetsPage::canSendAsset(asset, true));
    QVERIFY(B3AssetsPage::sendAssetDisabledReason(asset, true).contains(QStringLiteral("not yet mature")));
    asset.pending = 1;
    QVERIFY(!B3AssetsPage::canSendAsset(asset, true)); // Send uses confirmed inputs.
    asset.confirmed = asset.immature = 0;
    QVERIFY(!B3AssetsPage::canSendAsset(asset, true));
    QVERIFY(B3AssetsPage::sendAssetDisabledReason(asset, true).contains(QStringLiteral("awaiting confirmation")));
    asset.pending = 0;
    QVERIFY(!B3AssetsPage::canSendAsset(asset, true));
    QVERIFY(B3AssetsPage::sendAssetDisabledReason(asset, true).contains(QStringLiteral("no confirmed, mature balance")));
    asset.status = B3AssetRecord::Status::Unavailable;
    QVERIFY(!B3AssetsPage::canSendAsset(asset, true));
    asset.status = B3AssetRecord::Status::Native;
    QVERIFY(!B3AssetsPage::canSendAsset(asset, true));
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

    const QLabel* mesh_note{page.findChild<QLabel*>("assetFlowMeshReason")};
    QVERIFY(mesh_note);
    QVERIFY(mesh_note->isVisibleTo(&page));
    QCOMPARE(mesh_note->textFormat(), Qt::PlainText);
    QVERIFY(mesh_note->text().contains(QStringLiteral("pending successful market, deposit and withdrawal testing")));
    QVERIFY(mesh_note->text().contains(QStringLiteral("Activation height alone does not make a market ready")));
    QVERIFY(mesh_note->text().contains(QStringLiteral("keyless vault")));
    QVERIFY(mesh_note->text().contains(QStringLiteral("Trading remains disabled")));
    QVERIFY(!mesh_note->text().contains(QStringLiteral("Use the console")));
    QCOMPARE(deposit->toolTip(), mesh_note->text());
    QCOMPARE(withdraw->accessibleDescription(), mesh_note->text());

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
