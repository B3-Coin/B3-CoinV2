// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/test/b3splashtests.h>

#include <interfaces/node.h>
#include <qt/networkstyle.h>
#include <qt/splashscreen.h>
#include <util/chaintype.h>

#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QPixmap>
#include <QTest>

#include <memory>

namespace {
//! RAII guard forcing the reduced-motion environment flag.
class ReducedMotionGuard
{
public:
    explicit ReducedMotionGuard(bool on) : m_prev{qgetenv("B3_REDUCED_MOTION")}
    {
        qputenv("B3_REDUCED_MOTION", on ? "1" : "0");
    }
    ~ReducedMotionGuard() { qputenv("B3_REDUCED_MOTION", m_prev); }

private:
    QByteArray m_prev;
};

std::unique_ptr<const NetworkStyle> RegtestStyle()
{
    return std::unique_ptr<const NetworkStyle>(NetworkStyle::instantiate(ChainType::REGTEST));
}
} // namespace

void B3SplashTests::reducedMotionShowsStaticFrame()
{
    ReducedMotionGuard guard{true};
    auto style = RegtestStyle();
    QVERIFY(style != nullptr);
    SplashScreen splash(style.get());

    // Static fallback: no timer, and the complete final frame paints.
    QVERIFY(!splash.animationRunning());
    const QPixmap frame = splash.grab();
    QVERIFY(!frame.isNull());
}

void B3SplashTests::animationRunsAndStopsOnDestruction()
{
    ReducedMotionGuard guard{false};
    auto style = RegtestStyle();
    auto splash = std::make_unique<SplashScreen>(style.get());

    QVERIFY(splash->animationRunning());
    // Paint several animation frames, then destroy mid-animation: the
    // destructor must stop the timer with no use-after-free.
    for (int i = 0; i < 3; ++i) {
        QVERIFY(!splash->grab().isNull());
        QTest::qWait(40);
    }
    splash.reset();
    // Let any stray queued timer events drain; none may fire into the
    // destroyed widget.
    QTest::qWait(60);
}

void B3SplashTests::repeatedProgressUpdatesAreSafe()
{
    ReducedMotionGuard guard{true};
    auto style = RegtestStyle();
    SplashScreen splash(style.get());

    for (int i = 0; i <= 100; ++i) {
        splash.showMessage(QStringLiteral("Verifying blocks… %1%").arg(i),
                           Qt::AlignBottom | Qt::AlignHCenter, QColor(200, 205, 215));
    }
    QVERIFY(!splash.grab().isNull());
}

void B3SplashTests::closeDuringInitRequestsShutdown()
{
    ReducedMotionGuard guard{true};
    auto style = RegtestStyle();
    SplashScreen splash(style.get());
    splash.show();

    // Closing during initialization must be treated as a shutdown
    // request, not a window destruction: the event is ignored.
    QCloseEvent close_event;
    QApplication::sendEvent(&splash, &close_event);
    QVERIFY(!close_event.isAccepted());
    splash.hide();
}

void B3SplashTests::teardownWithNodeHandlersIsClean()
{
    ReducedMotionGuard guard{false};
    auto style = RegtestStyle();
    {
        SplashScreen splash(style.get());
        splash.setNode(m_node);
        splash.showMessage(QStringLiteral("Loading…"), Qt::AlignBottom | Qt::AlignHCenter,
                           QColor(200, 205, 215));
        QVERIFY(!splash.grab().isNull());
        // Destruction while subscribed to core signals and animating.
    }
    QTest::qWait(60);
}
