// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/test/b3assettests.h>

#include <qt/b3assetmodel.h>
#include <qt/b3assetspage.h>

#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

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
    record.decimals = 8;
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
        if (label->text().contains(QStringLiteral("no FlowMesh")) && label->isVisibleTo(&page)) {
            mesh_note = true;
        }
    }
    QVERIFY(mesh_note);

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
