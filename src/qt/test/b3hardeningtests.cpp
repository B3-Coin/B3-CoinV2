// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/test/b3hardeningtests.h>

#include <qt/b3assetspage.h>
#include <qt/b3chartwidget.h>
#include <qt/b3dashboardpage.h>
#include <qt/b3marketmodel.h>
#include <qt/b3navsidebar.h>
#include <qt/b3shell.h>
#include <qt/b3stakepage.h>
#include <qt/b3theme.h>
#include <qt/b3topstatus.h>
#include <qt/b3tradepage.h>
#include <qt/platformstyle.h>

#include <QLabel>
#include <QPushButton>
#include <QTest>
#include <QToolButton>

#include <cmath>
#include <memory>

namespace {

//! WCAG-style relative luminance.
double Luminance(const QColor& color)
{
    const auto channel = [](double c) {
        return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
}

double Contrast(const QColor& fg, const QColor& bg)
{
    const double l1 = std::max(Luminance(fg), Luminance(bg));
    const double l2 = std::min(Luminance(fg), Luminance(bg));
    return (l1 + 0.05) / (l2 + 0.05);
}

class AvailableBackend : public B3TradingBackend
{
public:
    using B3TradingBackend::B3TradingBackend;
    bool available() const override { return true; }
    QString unavailableReason() const override { return {}; }
    std::optional<qint64> estimatedFee(qint64, qint64) const override { return std::nullopt; }
};

} // namespace

void B3HardeningTests::sidebarKeyboardNavigationAndAccessibility()
{
    B3NavSidebar sidebar;

    const QStringList names{QStringLiteral("navDashboard"), QStringLiteral("navTrade"),
                            QStringLiteral("navAssets"), QStringLiteral("navStake"),
                            QStringLiteral("navActivity"), QStringLiteral("navSettings")};
    for (const QString& name : names) {
        auto* button = sidebar.findChild<QToolButton*>(name);
        QVERIFY2(button != nullptr, qPrintable(name));
        // Every destination is keyboard-reachable and announced.
        QCOMPARE(button->focusPolicy(), Qt::StrongFocus);
        QVERIFY(!button->accessibleName().isEmpty());
        QVERIFY(!button->accessibleDescription().isEmpty());
        // Activation via keyboard (space) works like a click.
    }

    auto* trade = sidebar.findChild<QToolButton*>("navTrade");
    trade->setFocus();
    QTest::keyClick(trade, Qt::Key_Space);
    QCOMPARE(sidebar.currentPage(), B3Page::Trade);
}

void B3HardeningTests::themeContrastIsReadable()
{
    // Dark-theme readability: primary text comfortably above the 4.5:1
    // body-text ratio, secondary text above 3:1 against cards too.
    QVERIFY(Contrast(B3Theme::kTextPrimary, B3Theme::kBackground) >= 4.5);
    QVERIFY(Contrast(B3Theme::kTextPrimary, B3Theme::kCard) >= 4.5);
    QVERIFY(Contrast(B3Theme::kTextSecondary, B3Theme::kBackground) >= 3.0);
    QVERIFY(Contrast(B3Theme::kTextSecondary, B3Theme::kCard) >= 3.0);
    // Status colors remain distinguishable from the background.
    QVERIFY(Contrast(B3Theme::kPositive, B3Theme::kBackground) >= 3.0);
    QVERIFY(Contrast(B3Theme::kNegative, B3Theme::kBackground) >= 3.0);
}

void B3HardeningTests::topStatusNetworkIdentitiesAreUnmistakable()
{
    B3TopStatus status;
    auto* badge = status.findChild<QLabel*>("B3NetBadge");
    QVERIFY(badge != nullptr);

    status.setNetwork(QStringLiteral("B3Coin"), QString());
    const QString main_text = badge->text();
    status.setNetwork(QStringLiteral("B3Coin"), QStringLiteral("[testnet]"));
    const QString test_text = badge->text();
    status.setNetwork(QStringLiteral("B3Coin"), QStringLiteral("[regtest]"));
    const QString reg_text = badge->text();

    // Three networks, three visibly different labels; non-mainnet shouts.
    QVERIFY(main_text != test_text && test_text != reg_text && main_text != reg_text);
    QCOMPARE(test_text, QStringLiteral("TESTNET"));
    QCOMPARE(reg_text, QStringLiteral("REGTEST"));
}

void B3HardeningTests::shellSurvivesWalletWidgetReplacementAndPageCycling()
{
    B3Shell shell;
    auto first = std::make_unique<QLabel>(QStringLiteral("wallet-one"));
    shell.setWalletWidget(first.get());

    // Replacement must not delete the externally-owned prior widget.
    auto second = std::make_unique<QLabel>(QStringLiteral("wallet-two"));
    shell.setWalletWidget(second.get());
    QVERIFY(!first->text().isEmpty());

    // Cycle every destination twice, including after the wallet widget
    // goes away mid-flight.
    for (int round = 0; round < 2; ++round) {
        for (B3Page page : {B3Page::Dashboard, B3Page::Trade, B3Page::Assets,
                            B3Page::Stake, B3Page::Activity, B3Page::Settings}) {
            shell.showPage(page);
            QCOMPARE(shell.sidebar()->currentPage(), page);
        }
        second.reset();
    }
}

void B3HardeningTests::tradeBackendReplacementFallsBackToNull()
{
    B3TradePage page;
    auto* submit = page.findChild<QPushButton*>("ticketSubmit");
    QVERIFY(!submit->isEnabled());

    // A live backend enables the primary action…
    auto backend = std::make_unique<AvailableBackend>();
    page.setBackend(backend.get());
    QVERIFY(submit->isEnabled());

    // …and its destruction must fall back to the null backend, disabled,
    // with no dangling pointer.
    backend.reset();
    QVERIFY(!submit->isEnabled());
    QVERIFY(page.backend() != nullptr);
    QVERIFY(!page.backend()->available());

    // Explicit null goes to the null backend as well.
    page.setBackend(nullptr);
    QVERIFY(!submit->isEnabled());
}

void B3HardeningTests::chartRendersExtremeValues()
{
    B3CandleSeries series;
    B3Candle extreme;
    extreme.timestamp = 1'900'000'000LL;
    extreme.low = 1;
    extreme.open = INT64_MAX / 2;
    extreme.close = INT64_MAX - 1;
    extreme.high = INT64_MAX - 1;
    extreme.volume = INT64_MAX - 1;
    series.setCandles({extreme});

    B3ChartWidget chart;
    chart.resize(400, 300);
    chart.setSeries(&series);
    QVERIFY(!chart.grab().isNull());

    // Tiny surface: still no crash, no divide-by-zero.
    chart.resize(chart.minimumSize());
    QVERIFY(!chart.grab().isNull());
    chart.setSeries(nullptr);
}

void B3HardeningTests::dashboardSurvivesModelChurn()
{
    std::unique_ptr<const PlatformStyle> style{PlatformStyle::instantiate("other")};
    auto page = std::make_unique<B3DashboardPage>(style.get());

    // Model churn with no backends: repeated null attach/detach, privacy
    // toggling and balance pushes must never dereference stale state.
    for (int i = 0; i < 3; ++i) {
        page->setClientModel(nullptr);
        page->setWalletModel(nullptr);
        page->setPrivacy(i % 2 == 0);
        interfaces::WalletBalances balances;
        balances.balance = i * 1'000'000LL;
        page->setBalance(balances);
        page->setNumConnections(i);
        page->setNetworkActive(i % 2 == 1);
    }
    QVERIFY(!page->grab().isNull());
    page.reset();
    QTest::qWait(20);
}

void B3HardeningTests::assetAndStakePagesSurviveWalletChurn()
{
    B3AssetsPage assets;
    B3StakePage stake;
    for (int i = 0; i < 3; ++i) {
        assets.setWalletModel(nullptr);
        stake.setWalletModel(nullptr);
    }
    QVERIFY(!assets.grab().isNull());
    QVERIFY(!stake.grab().isNull());
}
