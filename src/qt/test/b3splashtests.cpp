// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/test/b3splashtests.h>

#include <interfaces/node.h>
#include <node/interface_ui.h>
#include <qt/networkstyle.h>
#include <qt/splashscreen.h>
#include <util/chaintype.h>

#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace {
//! The supported Apple libc++ lacks jthread. Join also on a QTest early return,
//! before captured local state, node handlers or the splash can be destroyed.
class JoinedTestWorker
{
public:
    template <typename Fn>
    explicit JoinedTestWorker(Fn&& fn) : m_thread(std::forward<Fn>(fn)) {}
    ~JoinedTestWorker() { join(); }
    JoinedTestWorker(const JoinedTestWorker&) = delete;
    JoinedTestWorker& operator=(const JoinedTestWorker&) = delete;
    void join() { if (m_thread.joinable()) m_thread.join(); }

private:
    std::thread m_thread;
};

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

//! Observe painting without adding production hooks or touching node state.
class ObservedSplash final : public SplashScreen
{
public:
    using SplashScreen::SplashScreen;
    int paints{0};
    bool wrong_thread{false};

protected:
    void paintEvent(QPaintEvent* event) override
    {
        ++paints;
        wrong_thread |= QThread::currentThread() != qApp->thread();
        SplashScreen::paintEvent(event);
    }
};

QByteArray MeshFrameHash(SplashScreen& splash)
{
    // Exclude message/version/badge text. A changing message must not masquerade
    // as continued artwork animation. The mesh center is at (0.5, 0.42).
    const QImage frame = splash.grab(QRect{30, 60, 420, 135}).toImage().convertToFormat(QImage::Format_ARGB32);
    return QCryptographicHash::hash(QByteArray::fromRawData(
        reinterpret_cast<const char*>(frame.constBits()), static_cast<int>(frame.sizeInBytes())),
        QCryptographicHash::Sha256);
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

void B3SplashTests::delayedPhaseKeepsHeartbeat_data()
{
    QTest::addColumn<QString>("phase");
    QTest::addColumn<int>("delay_ms");
    QTest::addColumn<int>("reported_progress");
    QTest::newRow("fast-startup") << QStringLiteral("Loading configuration") << 250 << -1;
    QTest::newRow("delayed-block-index") << QStringLiteral("Loading block index") << 3100 << -1;
    QTest::newRow("delayed-wallet-load") << QStringLiteral("Loading generated test wallet") << 1200 << -1;
    QTest::newRow("verification") << QStringLiteral("Verifying blocks") << 1200 << 37;
    QTest::newRow("rescan") << QStringLiteral("Rescanning generated test wallet") << 1200 << 62;
}

void B3SplashTests::delayedPhaseKeepsHeartbeat()
{
    QFETCH(QString, phase);
    QFETCH(int, delay_ms);
    QFETCH(int, reported_progress);
    ReducedMotionGuard guard{false};
    auto style = RegtestStyle();
    ObservedSplash splash(style.get());
    splash.setNode(m_node);
    splash.show();
    auto* animation = splash.findChild<QTimer*>();
    QVERIFY(animation != nullptr);
    QSignalSpy frames(animation, &QTimer::timeout);
    QVERIFY(frames.isValid());

    int heartbeat_count{0};
    qint64 last_heartbeat{0};
    qint64 longest_heartbeat_gap{0};
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, &splash, [&] {
        const qint64 now = elapsed.elapsed();
        longest_heartbeat_gap = std::max(longest_heartbeat_gap, now - last_heartbeat);
        last_heartbeat = now;
        ++heartbeat_count;
    });
    heartbeat.start(50);

    std::atomic<bool> completed{false};
    std::atomic<bool> off_gui_thread{false};
    // Controlled worker delay, not a real index load or rescan. Exercise the
    // actual core-to-splash queued progress path while leaving Qt available.
    JoinedTestWorker worker([&] {
        off_gui_thread = QThread::currentThread() != qApp->thread();
        if (reported_progress < 0) {
            uiInterface.InitMessage(phase.toStdString());
        } else {
            uiInterface.ShowProgress(phase.toStdString(), reported_progress, true);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{delay_ms});
        completed = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(completed.load(), delay_ms + 3000);
    worker.join();
    heartbeat.stop();

    QVERIFY(off_gui_thread.load());
    QVERIFY(!splash.wrong_thread);
    QVERIFY(splash.animationRunning());
    QVERIFY(frames.count() >= 3);
    QVERIFY(splash.paints >= 3);
    QVERIFY(heartbeat_count >= 3);
    // A diagnostic bound, not a production frame-rate or startup SLA.
    QVERIFY2(longest_heartbeat_gap < 1000, "The controlled GUI heartbeat stalled for a second");
    qInfo().noquote() << QStringLiteral("splash_phase phase=%1 delay_ms=%2 elapsed_ms=%3 timer_frames=%4 paint_events=%5 heartbeat_count=%6 max_heartbeat_gap_ms=%7 progress=%8")
        .arg(phase).arg(delay_ms).arg(elapsed.elapsed()).arg(frames.count()).arg(splash.paints)
        .arg(heartbeat_count).arg(longest_heartbeat_gap)
        .arg(reported_progress < 0 ? QStringLiteral("indeterminate") : QString::number(reported_progress));
}

void B3SplashTests::idleFramesContinueAfterIntro()
{
    ReducedMotionGuard guard{false};
    auto style = RegtestStyle();
    ObservedSplash splash(style.get());
    splash.show();
    QTest::qWait(2400); // Existing intro ends at 2200 ms; no production timing is changed.
    auto* animation = splash.findChild<QTimer*>();
    QVERIFY(animation != nullptr);
    QCOMPARE(animation->interval(), 100);
    QSignalSpy frames(animation, &QTimer::timeout);
    QSet<QByteArray> distinct_frames;
    for (int i = 0; i < 6; ++i) {
        distinct_frames.insert(MeshFrameHash(splash));
        QTest::qWait(250);
    }
    QVERIFY(splash.animationRunning());
    QVERIFY(frames.count() >= 5);
    QVERIFY2(distinct_frames.size() > 1, "The mesh stopped changing after the finite intro");
    QVERIFY(!splash.wrong_thread);
    qInfo() << "splash_idle distinct_mesh_frames=" << distinct_frames.size()
            << "timer_frames=" << frames.count() << "paint_events=" << splash.paints;
}

void B3SplashTests::reducedMotionDoesNotPulseDuringDelay()
{
    ReducedMotionGuard guard{true};
    auto style = RegtestStyle();
    SplashScreen splash(style.get());
    splash.show();
    splash.showMessage(QStringLiteral("Loading block index"), Qt::AlignBottom | Qt::AlignHCenter, Qt::white);
    const auto first = MeshFrameHash(splash);
    QTest::qWait(350);
    QCOMPARE(MeshFrameHash(splash), first);
    QVERIFY(!splash.animationRunning());
    splash.showMessage(QStringLiteral("Verifying blocks: 37%"), Qt::AlignBottom | Qt::AlignHCenter, Qt::white);
    QCOMPARE(MeshFrameHash(splash), first);
}

void B3SplashTests::queuedProgressIsDiscardedAfterDestruction()
{
    ReducedMotionGuard guard{false};
    auto style = RegtestStyle();
    auto splash = std::make_unique<SplashScreen>(style.get());
    splash->setNode(m_node);
    QPointer<SplashScreen> lifetime(splash.get());
    QPointer<QTimer> animation(splash->findChild<QTimer*>());
    QVERIFY(animation != nullptr);
    JoinedTestWorker worker([] {
        uiInterface.InitMessage("Queued test progress before destruction");
        uiInterface.ShowProgress("Queued verification", 37, true);
    });
    worker.join();
    // Do not pump Qt before destruction: both messages are still queued.
    splash.reset();
    QVERIFY(lifetime.isNull());
    QVERIFY(animation.isNull());
    // Disconnected core callbacks and queued calls must not touch the widget.
    uiInterface.InitMessage("Test progress after destruction");
    uiInterface.ShowProgress("Test verification after destruction", 62, true);
    QTest::qWait(150);
}

void B3SplashTests::controlledStartupEndStopsTimers_data()
{
    QTest::addColumn<QString>("outcome");
    QTest::newRow("recoverable-error-then-ready") << QStringLiteral("recoverable");
    QTest::newRow("fatal-error") << QStringLiteral("fatal");
    QTest::newRow("user-cancel") << QStringLiteral("cancel");
    QTest::newRow("main-window-ready") << QStringLiteral("ready");
}

void B3SplashTests::controlledStartupEndStopsTimers()
{
    QFETCH(QString, outcome);
    ReducedMotionGuard guard{false};
    auto style = RegtestStyle();
    auto splash = std::make_unique<SplashScreen>(style.get());
    splash->show();
    QPointer<QTimer> animation(splash->findChild<QTimer*>());
    QVERIFY(animation != nullptr);
    QTest::qWait(60);
    if (outcome == QStringLiteral("recoverable")) {
        splash->showMessage(QStringLiteral("A test initialization warning was handled; continuing"),
                            Qt::AlignBottom | Qt::AlignHCenter, Qt::white);
        QTest::qWait(120);
        QVERIFY(splash->animationRunning());
    } else if (outcome == QStringLiteral("fatal")) {
        splash->showMessage(QStringLiteral("Fatal test initialization error"),
                            Qt::AlignBottom | Qt::AlignHCenter, Qt::red);
        QVERIFY(!splash->grab().isNull());
    } else if (outcome == QStringLiteral("cancel")) {
        // Cancel requests safe shutdown; the splash remains owned until the
        // application ends initialization. This is not a database cancellation.
        QCloseEvent event;
        QApplication::sendEvent(splash.get(), &event);
        QVERIFY(!event.isAccepted());
    }
    QWidget ready_window;
    if (outcome == QStringLiteral("ready") || outcome == QStringLiteral("recoverable")) {
        ready_window.show();
        QVERIFY(ready_window.isVisible());
    }
    // Model only the existing owner-destruction endpoint. This does not replace
    // an attended full-app startup/fatal-dialog qualification.
    splash.reset();
    QVERIFY(animation.isNull());
    QTest::qWait(150);
}
