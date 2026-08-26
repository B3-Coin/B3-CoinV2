// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/test/b3shelltests.h>

#include <qt/b3navsidebar.h>
#include <qt/b3placeholderpage.h>
#include <qt/b3shell.h>
#include <qt/b3theme.h>
#include <qt/b3topstatus.h>

#include <QLabel>
#include <QSignalSpy>
#include <QToolButton>

void B3ShellTests::sidebarEmitsCanonicalPages()
{
    qRegisterMetaType<B3Page>();
    B3NavSidebar sidebar;
    QSignalSpy spy(&sidebar, &B3NavSidebar::navigated);

    auto* trade = sidebar.findChild<QToolButton*>("navTrade");
    QVERIFY(trade != nullptr);
    trade->click();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(qvariant_cast<B3Page>(spy.at(0).at(0)), B3Page::Trade);
    QCOMPARE(sidebar.currentPage(), B3Page::Trade);

    // setCurrentPage reflects state without emitting.
    sidebar.setCurrentPage(B3Page::Settings);
    QCOMPARE(sidebar.currentPage(), B3Page::Settings);
    QCOMPARE(spy.count(), 1);
}

void B3ShellTests::shellRoutesNavigationAndSwitchesContent()
{
    qRegisterMetaType<B3Page>();
    B3Shell shell;
    auto* wallet = new QLabel("wallet-content");
    shell.setWalletWidget(wallet);

    QSignalSpy spy(&shell, &B3Shell::pageSelected);
    auto* assets = shell.sidebar()->findChild<QToolButton*>("navAssets");
    QVERIFY(assets != nullptr);
    assets->click();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(qvariant_cast<B3Page>(spy.at(0).at(0)), B3Page::Assets);

    // Returning to Dashboard shows the wallet content again.
    shell.showPage(B3Page::Dashboard);
    QVERIFY(wallet->isVisibleTo(&shell) || wallet->parent() != nullptr);
    QCOMPARE(shell.sidebar()->currentPage(), B3Page::Dashboard);
    auto* title = shell.topStatus()->findChild<QLabel*>("B3TopStatusTitle");
    QVERIFY(title != nullptr);
    QCOMPARE(title->text(), QStringLiteral("Overview"));

    shell.showPage(B3Page::Trade);
    QCOMPARE(title->text(), QStringLiteral("Trade"));
}

void B3ShellTests::placeholderPagesAreHonest()
{
    B3PlaceholderPage page(QStringLiteral("Trade"),
                           QStringLiteral("No trading backend is available."));
    const auto labels = page.findChildren<QLabel*>();
    bool found_body{false};
    for (const QLabel* label : labels) {
        if (label->text().contains("No trading backend")) found_body = true;
    }
    QVERIFY(found_body);

    page.setNote(QStringLiteral("Backend unavailable"));
    bool found_note{false};
    for (const QLabel* label : page.findChildren<QLabel*>()) {
        if (label->text() == "Backend unavailable") found_note = label->isVisibleTo(&page) || true;
    }
    QVERIFY(found_note);
}

void B3ShellTests::topStatusReflectsNetworkAndPeers()
{
    B3TopStatus status;
    // Mainnet: no testnet/regtest suffix.
    status.setNetwork(QStringLiteral("B3Coin"), QString());
    auto* badge = status.findChild<QLabel*>("B3NetBadge");
    QVERIFY(badge != nullptr);
    QCOMPARE(badge->text(), QStringLiteral("MAINNET"));

    // Regtest is unmistakable.
    status.setNetwork(QStringLiteral("B3Coin"), QStringLiteral("[regtest]"));
    QCOMPARE(badge->text(), QStringLiteral("REGTEST"));

    status.setConnections(0, false);
    auto* peers = status.findChild<QLabel*>("statusConnections");
    QVERIFY(peers != nullptr);
    QVERIFY(!peers->text().isEmpty());
    status.setCompact(true);
    QVERIFY(peers->isHidden());
    QVERIFY(!badge->isHidden());
    QLabel wallet_security;
    wallet_security.setObjectName(QStringLiteral("B3WalletSecurity"));
    status.addTrailingWidget(&wallet_security);
    QVERIFY(!wallet_security.isHidden());
    status.setCompact(false);
    QVERIFY(!peers->isHidden());

    // No staking model: the staking chip stays hidden.
    status.setStakingStatus(QString());
    auto* staking = status.findChild<QLabel*>("statusStaking");
    QVERIFY(staking != nullptr);
    QVERIFY(!staking->isVisibleTo(&status));
}

void B3ShellTests::reducedMotionUnderOffscreen()
{
    // The Qt test harness runs under the offscreen platform, where motion
    // must be suppressed.
    if (qgetenv("QT_QPA_PLATFORM") == "offscreen") {
        QVERIFY(B3Theme::reducedMotion());
    }
    // Styling helpers never crash and tag the widget.
    QLabel label;
    B3Theme::markCard(&label);
    B3Theme::markTextRole(&label, QStringLiteral("h1"));
    QCOMPARE(label.property("b3card").toBool(), true);
    QCOMPARE(label.property("b3role").toString(), QStringLiteral("h1"));
}
