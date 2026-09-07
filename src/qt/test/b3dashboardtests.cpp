// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/test/b3dashboardtests.h>

#include <consensus/amount.h>
#include <qt/b3dashboardpage.h>
#include <qt/b3validatorcontroller.h>
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

void B3DashboardTests::immatureRowReflectsActualBalance()
{
    auto style = TestStyle();
    B3DashboardPage page(style.get());

    interfaces::WalletBalances balances;
    balances.balance = 5 * COIN;
    balances.immature_balance = 0;
    page.setBalance(balances);
    // The approved wallet card always carries the row; zero is a real value,
    // not fabricated data or an unavailable state.
    int visible_immature{0};
    for (const QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("Immature") && label->isVisibleTo(&page)) ++visible_immature;
    }
    QCOMPARE(visible_immature, 1);

    balances.immature_balance = COIN;
    page.setBalance(balances);
    visible_immature = 0;
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
    QVERIFY(progress->isVisibleTo(&page));
    QCOMPARE(progress->value(), 1000);
    auto* era_height = page.findChild<QLabel*>(QStringLiteral("dashboardEraHeight"));
    QVERIFY(era_height != nullptr);
    const QString connected_height{era_height->text()};

    // A header tip can be far ahead; it must not masquerade as the connected
    // chain height or change the era card.
    page.setNumBlocks(900, QDateTime::currentDateTime(), 1.0, SyncType::HEADER_SYNC, SynchronizationState::INIT_DOWNLOAD);
    QCOMPARE(era_height->text(), connected_height);

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

void B3DashboardTests::stakingCardUsesVerifiedControllerStatus()
{
    auto style = TestStyle();
    B3DashboardPage page(style.get());
    auto* card = page.findChild<QWidget*>(QStringLiteral("dashboardStakingCard"));
    auto* text = page.findChild<QLabel*>(QStringLiteral("dashboardStakingStatus"));
    QVERIFY(card && text);
    QVERIFY(!card->isHidden());
    QCOMPARE(text->textFormat(), Qt::PlainText);
    QVERIFY(text->text().contains(QStringLiteral("Select a wallet")));

    B3ValidatorStatus status;
    status.valid = true;
    status.staking_running = true;
    status.staking_uses_this_wallet = true;
    status.finality_signing = true;
    status.last_signed_height = 814461;
    page.setValidatorStatus(status);
    QVERIFY(text->text().contains(QStringLiteral("Staking is running")));
    QVERIFY(text->text().contains(QStringLiteral("814461")));

    status.staking_uses_this_wallet = false;
    page.setValidatorStatus(status);
    QVERIFY(text->text().contains(QStringLiteral("Another wallet")));
    QVERIFY(!text->text().contains(QStringLiteral("814461")));

    status.valid = false;
    status.refresh_error = QStringLiteral("<b>test error</b>");
    page.setValidatorStatus(status);
    QVERIFY(text->text().contains(QStringLiteral("unavailable")));
    QVERIFY(!text->text().contains(QStringLiteral("Staking is running")));
    QCOMPARE(text->textFormat(), Qt::PlainText);
    page.setWalletModel(nullptr);
    QVERIFY(text->text().contains(QStringLiteral("Select a wallet")));
}
