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
#include <QStackedWidget>
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
    auto* content = shell.findChild<QStackedWidget*>("B3Content");
    QVERIFY(content != nullptr);
    QCOMPARE(content->currentWidget(), wallet->parentWidget());
    QVERIFY(wallet->isVisibleTo(&shell));
    QCOMPARE(shell.sidebar()->currentPage(), B3Page::Dashboard);
    auto* title = shell.topStatus()->findChild<QLabel*>("B3TopStatusTitle");
    QVERIFY(title != nullptr);
    QCOMPARE(title->text(), QStringLiteral("Overview"));

    shell.showPage(B3Page::Trade);
    QCOMPARE(title->text(), QStringLiteral("Trade"));
}

void B3ShellTests::everyDestinationHasMatchingTitleAndSelection_data()
{
    QTest::addColumn<B3Page>("page");
    QTest::addColumn<QString>("button_name");
    QTest::addColumn<QString>("title");
    QTest::addColumn<bool>("wallet_page");
    QTest::newRow("overview") << B3Page::Dashboard << QStringLiteral("navDashboard") << QStringLiteral("Overview") << true;
    QTest::newRow("send") << B3Page::Send << QStringLiteral("navSend") << QStringLiteral("Send") << true;
    QTest::newRow("receive") << B3Page::Receive << QStringLiteral("navReceive") << QStringLiteral("Receive") << true;
    QTest::newRow("activity") << B3Page::Activity << QStringLiteral("navActivity") << QStringLiteral("Activity") << true;
    QTest::newRow("trade") << B3Page::Trade << QStringLiteral("navTrade") << QStringLiteral("Trade") << false;
    QTest::newRow("assets") << B3Page::Assets << QStringLiteral("navAssets") << QStringLiteral("Assets") << false;
    QTest::newRow("stake") << B3Page::Stake << QStringLiteral("navStake") << QStringLiteral("Stake") << false;
    QTest::newRow("settings") << B3Page::Settings << QStringLiteral("navSettings") << QStringLiteral("Settings") << false;
}

void B3ShellTests::everyDestinationHasMatchingTitleAndSelection()
{
    QFETCH(B3Page, page);
    QFETCH(QString, button_name);
    QFETCH(QString, title);
    QFETCH(bool, wallet_page);
    B3Shell shell;
    shell.setWalletWidget(new QLabel(QStringLiteral("wallet-content")));
    shell.setSettingsPage(new QLabel(QStringLiteral("settings-content")));
    QSignalSpy navigated(&shell, &B3Shell::pageSelected);
    auto* button = shell.sidebar()->findChild<QToolButton*>(button_name);
    auto* heading = shell.topStatus()->findChild<QLabel*>("B3TopStatusTitle");
    QVERIFY(button != nullptr);
    QVERIFY(heading != nullptr);
    button->click();
    QCOMPARE(navigated.count(), 1);
    QCOMPARE(qvariant_cast<B3Page>(navigated.at(0).at(0)), page);
    QCOMPARE(shell.currentPage(), page);
    QCOMPARE(shell.sidebar()->currentPage(), page);
    QCOMPARE(heading->text(), title);
    QCOMPARE(shell.walletPageVisible(), wallet_page);
    QVERIFY(button->isChecked());
    QVERIFY(!button->icon().isNull());
    QCOMPARE(button->accessibleName(), title);

    shell.sidebar()->setCompact(true);
    QCOMPARE(button->toolTip(), title);
    QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
    QVERIFY(button->isChecked());
}

void B3ShellTests::disabledNavigationCannotBypassWalletActionPolicy()
{
    B3Shell shell;
    shell.showPage(B3Page::Send);
    QSignalSpy navigated(&shell, &B3Shell::pageSelected);
    auto* activity = shell.sidebar()->findChild<QToolButton*>("navActivity");
    QVERIFY(activity != nullptr);
    shell.sidebar()->setPageEnabled(B3Page::Activity, false);
    QVERIFY(!shell.sidebar()->isPageEnabled(B3Page::Activity));
    activity->click();
    QCOMPARE(navigated.count(), 0);
    shell.showPage(B3Page::Activity);
    QCOMPARE(shell.currentPage(), B3Page::Send);
    QCOMPARE(shell.sidebar()->currentPage(), B3Page::Send);
    shell.sidebar()->setPageEnabled(B3Page::Activity, true);
    activity->click();
    QCOMPARE(navigated.count(), 1);
    QCOMPARE(shell.currentPage(), B3Page::Activity);
}

void B3ShellTests::settingsDialogFallbackPreservesVisiblePage()
{
    B3Shell shell;
    shell.showPage(B3Page::Trade);
    auto* content = shell.findChild<QStackedWidget*>("B3Content");
    auto* heading = shell.topStatus()->findChild<QLabel*>("B3TopStatusTitle");
    auto* settings = shell.sidebar()->findChild<QToolButton*>("navSettings");
    QVERIFY(content != nullptr);
    QVERIFY(heading != nullptr);
    QVERIFY(settings != nullptr);
    QWidget* previous = content->currentWidget();
    QSignalSpy navigated(&shell, &B3Shell::pageSelected);
    settings->click();
    // The request still reaches the window to open the options dialog.
    QCOMPARE(navigated.count(), 1);
    QCOMPARE(qvariant_cast<B3Page>(navigated.at(0).at(0)), B3Page::Settings);
    QCOMPARE(content->currentWidget(), previous);
    QCOMPARE(heading->text(), QStringLiteral("Trade"));
    QCOMPARE(shell.currentPage(), B3Page::Trade);
    QCOMPARE(shell.sidebar()->currentPage(), B3Page::Trade);
}

void B3ShellTests::replacingVisiblePagesPreservesSelection()
{
    B3Shell shell;
    auto* content = shell.findChild<QStackedWidget*>("B3Content");
    QVERIFY(content != nullptr);
    shell.showPage(B3Page::Trade);
    auto* trade = new QLabel(QStringLiteral("live-trade-body"));
    shell.setTradePage(trade);
    QCOMPARE(content->currentWidget(), trade);
    QCOMPARE(shell.currentPage(), B3Page::Trade);
    const int page_count = content->count();
    shell.setTradePage(trade);
    QCOMPARE(content->currentWidget(), trade);
    QCOMPARE(content->count(), page_count);
    shell.setAssetsPage(new QLabel(QStringLiteral("asset-body")));
    QCOMPARE(content->currentWidget(), trade);
    shell.setSettingsPage(new QLabel(QStringLiteral("settings-body")));
    shell.showPage(B3Page::Settings);
    auto* settings = new QLabel(QStringLiteral("replacement-settings-body"));
    shell.setSettingsPage(settings);
    QCOMPARE(content->currentWidget(), settings);
    QCOMPARE(shell.currentPage(), B3Page::Settings);
}

void B3ShellTests::sidebarDoesNotInventNetworkFeatureStatus()
{
    B3NavSidebar sidebar;
    const auto* footer = sidebar.findChild<QLabel*>("B3SidebarPlatform");
    QVERIFY(footer != nullptr);
    QCOMPARE(footer->text(), QStringLiteral("B3 HIVE DESKTOP"));
    QVERIFY(!footer->accessibleName().contains(QStringLiteral("inactive"), Qt::CaseInsensitive));
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
        if (label->text() == "Backend unavailable") found_note = label->isVisibleTo(&page);
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
