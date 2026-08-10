// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/test/b3dashboardtests.h>

#include <consensus/amount.h>
#include <qt/b3dashboardpage.h>
#include <qt/bitcoinunits.h>
#include <qt/platformstyle.h>

#include <interfaces/wallet.h>
#include <validation.h>

#include <QDateTime>
#include <QLabel>
#include <QListView>
#include <QProgressBar>
#include <QPushButton>
#include <QTest>

#include <memory>

namespace {
std::unique_ptr<const PlatformStyle> TestStyle()
{
    return std::unique_ptr<const PlatformStyle>(PlatformStyle::instantiate("other"));
}
} // namespace

void B3DashboardTests::noWalletShowsHonestEmptyState()
{
    auto style = TestStyle();
    B3DashboardPage page(style.get());

    // Without a wallet model: actions disabled, honest note shown, no
    // balance values fabricated.
    auto* send = page.findChild<QPushButton*>("dashboardSend");
    auto* receive = page.findChild<QPushButton*>("dashboardReceive");
    QVERIFY(send && receive);
    QVERIFY(!send->isEnabled());
    QVERIFY(!receive->isEnabled());

    bool found_no_wallet{false};
    for (const QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("No wallet"))) found_no_wallet = true;
    }
    QVERIFY(found_no_wallet);

    // Explicit null model assignment is a no-op, not a crash.
    page.setWalletModel(nullptr);
    page.setClientModel(nullptr);
}

void B3DashboardTests::balancesRenderLargeValuesAndPrivacy()
{
    // The formatting path used by every dashboard money label.
    constexpr CAmount large{2'100'000'000'000'000LL};
    const QString shown = B3DashboardPage::formatAmount(BitcoinUnit::BTC, large, /*privacy=*/false);
    QVERIFY(!shown.isEmpty());
    QCOMPARE(shown, BitcoinUnits::formatWithPrivacy(BitcoinUnit::BTC, large, BitcoinUnits::SeparatorStyle::ALWAYS, false));

    const QString masked = B3DashboardPage::formatAmount(BitcoinUnit::BTC, large, /*privacy=*/true);
    QVERIFY(masked != shown);
    QVERIFY(!masked.contains(QStringLiteral("21")));

    // A page fed balances via the model slot renders them (and privacy
    // re-renders them masked) without a wallet backend.
    auto style = TestStyle();
    B3DashboardPage page(style.get());
    interfaces::WalletBalances balances;
    balances.balance = large;
    balances.unconfirmed_balance = 1;
    page.setBalance(balances);

    bool found_value{false};
    for (const QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text() == shown) found_value = true;
    }
    QVERIFY(found_value);

    page.setPrivacy(true);
    bool found_unmasked{false};
    for (const QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text() == shown) found_unmasked = true;
    }
    QVERIFY(!found_unmasked);
}

void B3DashboardTests::immatureRowOnlyWhenNonZero()
{
    auto style = TestStyle();
    B3DashboardPage page(style.get());

    interfaces::WalletBalances balances;
    balances.balance = 5 * COIN;
    balances.immature_balance = 0;
    page.setBalance(balances);
    // The immature row stays hidden when there is nothing immature.
    int visible_immature{0};
    for (const QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("Immature") && label->isVisibleTo(&page)) ++visible_immature;
    }
    QCOMPARE(visible_immature, 0);

    balances.immature_balance = COIN;
    page.setBalance(balances);
    for (const QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("Immature") && label->isVisibleTo(&page)) ++visible_immature;
    }
    QCOMPARE(visible_immature, 1);
}

void B3DashboardTests::privacyMasksActivity()
{
    auto style = TestStyle();
    B3DashboardPage page(style.get());

    page.setPrivacy(true);
    auto* activity = page.findChild<QListView*>("dashboardActivity");
    QVERIFY(activity != nullptr);
    QVERIFY(!activity->isVisibleTo(&page));

    bool masked_note_visible{false};
    for (const QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("masked")) && label->isVisibleTo(&page)) masked_note_visible = true;
    }
    QVERIFY(masked_note_visible);

    page.setPrivacy(false);
    // With no wallet there are no rows, so the empty state returns.
    bool empty_note_visible{false};
    for (const QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("No transactions")) && label->isVisibleTo(&page)) empty_note_visible = true;
    }
    QVERIFY(empty_note_visible);
}

void B3DashboardTests::clientViewSlotsRenderWithoutNode()
{
    auto style = TestStyle();
    B3DashboardPage page(style.get());

    // Repeated progress updates are safe and reflected honestly.
    page.setNumBlocks(100, QDateTime::currentDateTime(), 0.25, SyncType::BLOCK_SYNC, SynchronizationState::INIT_DOWNLOAD);
    auto* progress = page.findChild<QProgressBar*>();
    QVERIFY(progress != nullptr);
    QCOMPARE(progress->value(), 250);
    QVERIFY(progress->isVisibleTo(&page));

    page.setNumBlocks(400, QDateTime::currentDateTime(), 1.0, SyncType::BLOCK_SYNC, SynchronizationState::POST_INIT);
    QVERIFY(!progress->isVisibleTo(&page));

    page.setNumConnections(0);
    page.setNetworkActive(false);
    page.setNumConnections(8);
    page.setNetworkActive(true);

    page.showOutOfSyncWarning(true);
    bool warning_visible{false};
    for (const QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("out of date")) && label->isVisibleTo(&page)) warning_visible = true;
    }
    QVERIFY(warning_visible);
    page.showOutOfSyncWarning(false);
}
