// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <qt/splashscreen.h>
#include <qt/guiconstants.h>

#include <clientversion.h>
#include <common/system.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <qt/b3theme.h>
#include <qt/guiutil.h>
#include <qt/networkstyle.h>
#include <qt/walletmodel.h>
#include <util/translation.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>

#include <QApplication>
#include <QCloseEvent>
#include <QPainter>
#include <QScreen>
#include <QTimer>

namespace {
// Animation timeline (milliseconds since show).
constexpr int kNodesStartMs{150};
constexpr int kNodeFadeMs{500};
constexpr int kNodeStaggerMs{60};
constexpr int kEdgesStartMs{700};
constexpr int kEdgeFadeMs{900};
constexpr int kMarkStartMs{1400};
constexpr int kMarkFadeMs{600};
constexpr int kIntroEndMs{2200};
// Subtle idle pulse: low frequency, small amplitude, low frame rate.
constexpr double kPulseHz{0.25};
constexpr double kPulseAmplitude{0.05};
constexpr int kIntroFrameMs{33};
constexpr int kIdleFrameMs{100};

constexpr QSize kSplashSize{480, 320};
constexpr QPointF kMeshCenter{0.5, 0.42};
} // namespace

SplashScreen::SplashScreen(const NetworkStyle* networkStyle)
    : QWidget()
{
    m_reduced_motion = B3Theme::reducedMotion();

    buildBackground(networkStyle);
    buildMesh();

    // The approved B3 mark that the mesh converges around; the artwork
    // itself stays replaceable in the Qt resource system via NetworkStyle.
    const qreal dpr = devicePixelRatioF();
    m_mark = networkStyle->getAppIcon().pixmap(QSize(96 * dpr, 96 * dpr));
    m_mark.setDevicePixelRatio(dpr);

    // Set window title
    setWindowTitle(QString(HIVE_NAME) + " " + networkStyle->getTitleAddText());

    // Resize window and move to center of desktop, disallow resizing
    QRect r(QPoint(), kSplashSize);
    resize(r.size());
    setFixedSize(r.size());
    move(QGuiApplication::primaryScreen()->geometry().center() - r.center());

    installEventFilter(this);

    if (!m_reduced_motion) {
        m_clock.start();
        m_anim_timer = new QTimer(this);
        connect(m_anim_timer, &QTimer::timeout, this, [this] { animationTick(); });
        m_anim_timer->start(kIntroFrameMs);
    }

    GUIUtil::handleCloseWindowShortcut(this);
}

void SplashScreen::buildBackground(const NetworkStyle* networkStyle)
{
    const QString versionText = QString("Version %1").arg(QString::fromStdString(FormatFullVersion()));
    const QString copyrightText = QString::fromUtf8(CopyrightHolders(strprintf("\xc2\xA9 %u-%u ", 2009, COPYRIGHT_YEAR)).c_str());
    const QString& titleAddText = networkStyle->getTitleAddText();
    const QString font = QApplication::font().toString();

    const qreal dpr = static_cast<QGuiApplication*>(QCoreApplication::instance())->devicePixelRatio();
    pixmap = QPixmap(kSplashSize * dpr);
    pixmap.setDevicePixelRatio(dpr);

    QPainter pixPaint(&pixmap);
    pixPaint.fillRect(QRect(QPoint(0, 0), kSplashSize), B3Theme::kBackground);

    // Version + copyright, bottom left, quiet.
    pixPaint.setPen(B3Theme::kTextMuted);
    pixPaint.setFont(QFont(font, 9));
    pixPaint.drawText(QRect(16, kSplashSize.height() - 64, kSplashSize.width() - 32, 24),
                      Qt::AlignLeft | Qt::AlignBottom, versionText);
    pixPaint.drawText(QRect(16, kSplashSize.height() - 44, kSplashSize.width() - 32, 40),
                      Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, copyrightText);

    // Unmistakable network badge for non-mainnet chains, top right.
    if (!titleAddText.isEmpty()) {
        const bool regtest = titleAddText.contains("regtest", Qt::CaseInsensitive);
        const QColor badge = regtest ? B3Theme::kRegtest : B3Theme::kWarning;
        QFont boldFont(font, 11);
        boldFont.setWeight(QFont::Bold);
        pixPaint.setFont(boldFont);
        QString network = titleAddText.toUpper();
        network.remove('[');
        network.remove(']');
        const QFontMetrics fm(boldFont);
        const int w = GUIUtil::TextWidth(fm, network) + 20;
        const QRect badgeRect(kSplashSize.width() - w - 14, 14, w, fm.height() + 8);
        pixPaint.setPen(Qt::NoPen);
        pixPaint.setBrush(badge);
        pixPaint.setRenderHint(QPainter::Antialiasing);
        pixPaint.drawRoundedRect(badgeRect, 6, 6);
        pixPaint.setPen(B3Theme::kBackground);
        pixPaint.drawText(badgeRect, Qt::AlignCenter, network);
    }

    pixPaint.end();
}

void SplashScreen::buildMesh()
{
    // Deterministic layout: scattered start positions drift into two
    // rings converging around the mark. No runtime randomness, so every
    // start looks identical and tests are reproducible.
    static constexpr struct { double fx, fy; } kFrom[] = {
        {0.05, 0.10}, {0.92, 0.06}, {0.15, 0.85}, {0.85, 0.90}, {0.50, 0.02},
        {0.03, 0.50}, {0.97, 0.45}, {0.30, 0.95}, {0.70, 0.03}, {0.10, 0.30},
        {0.90, 0.70}, {0.40, 0.08}, {0.60, 0.95}, {0.05, 0.70}, {0.95, 0.25},
        {0.25, 0.05}, {0.75, 0.92}, {0.50, 0.98},
    };
    const int n = static_cast<int>(std::size(kFrom));
    m_nodes.clear();
    m_nodes.reserve(n);
    for (int i = 0; i < n; ++i) {
        const bool inner = (i % 3 == 0);
        const double radius = inner ? 0.16 : 0.26;
        const double angle = (2.0 * M_PI * i) / n;
        MeshNode node;
        node.from = QPointF(kFrom[i].fx, kFrom[i].fy);
        node.to = QPointF(kMeshCenter.x() + radius * std::cos(angle),
                          kMeshCenter.y() + radius * std::sin(angle) * 0.85);
        node.fade_start_ms = kNodesStartMs + i * kNodeStaggerMs;
        m_nodes.push_back(node);
    }

    // Connect each node to its ring neighbours and a few spokes across.
    m_edges.clear();
    for (int i = 0; i < n; ++i) {
        m_edges.emplace_back(i, (i + 1) % n);
        if (i % 3 == 0) m_edges.emplace_back(i, (i + n / 2) % n);
    }
}

qreal SplashScreen::phaseProgress(qint64 now_ms, int start_ms, int duration_ms)
{
    if (now_ms <= start_ms) return 0.0;
    if (now_ms >= start_ms + duration_ms) return 1.0;
    const qreal linear = static_cast<qreal>(now_ms - start_ms) / duration_ms;
    // Ease in/out for calm motion.
    return linear * linear * (3.0 - 2.0 * linear);
}

bool SplashScreen::animationRunning() const
{
    return m_anim_timer && m_anim_timer->isActive();
}

void SplashScreen::animationTick()
{
    // After the intro, drop to a low idle frame rate: the remaining
    // motion is only the subtle mesh pulse.
    if (!m_idle_rate && m_clock.elapsed() > kIntroEndMs) {
        m_idle_rate = true;
        m_anim_timer->setInterval(kIdleFrameMs);
    }
    update();
}

SplashScreen::~SplashScreen()
{
    // Stop the animation before teardown so no timer fires into a
    // partially-destroyed widget.
    if (m_anim_timer) m_anim_timer->stop();
    if (m_node) unsubscribeFromCoreSignals();
}

void SplashScreen::setNode(interfaces::Node& node)
{
    assert(!m_node);
    m_node = &node;
    subscribeToCoreSignals();
    if (m_shutdown) m_node->startShutdown();
}

void SplashScreen::shutdown()
{
    m_shutdown = true;
    if (m_node) m_node->startShutdown();
}

bool SplashScreen::eventFilter(QObject * obj, QEvent * ev) {
    if (ev->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(ev);
        if (keyEvent->key() == Qt::Key_Q) {
            shutdown();
        }
    }
    return QObject::eventFilter(obj, ev);
}

static void InitMessage(SplashScreen *splash, const std::string &message)
{
    bool invoked = QMetaObject::invokeMethod(splash, "showMessage",
        Qt::QueuedConnection,
        Q_ARG(QString, QString::fromStdString(message)),
        Q_ARG(int, Qt::AlignBottom|Qt::AlignHCenter),
        Q_ARG(QColor, B3Theme::kTextSecondary));
    assert(invoked);
}

static void ShowProgress(SplashScreen *splash, const std::string &title, int nProgress, bool resume_possible)
{
    InitMessage(splash, title + std::string("\n") +
            (resume_possible ? SplashScreen::tr("(press q to shutdown and continue later)").toStdString()
                                : SplashScreen::tr("press q to shutdown").toStdString()) +
            strprintf("\n%d", nProgress) + "%");
}

void SplashScreen::subscribeToCoreSignals()
{
    // Connect signals to client
    m_handler_init_message = m_node->handleInitMessage([this](const std::string& message) {
        InitMessage(this, message);
    });
    m_handler_show_progress = m_node->handleShowProgress([this](const std::string& title, int nProgress, bool resume_possible) {
        ShowProgress(this, title, nProgress, resume_possible);
    });
    m_handler_init_wallet = m_node->handleInitWallet([this]() { handleLoadWallet(); });
}

void SplashScreen::handleLoadWallet()
{
#ifdef ENABLE_WALLET
    if (!WalletModel::isWalletEnabled()) return;
    m_handler_load_wallet = m_node->walletLoader().handleLoadWallet([this](std::unique_ptr<interfaces::Wallet> wallet) {
        m_connected_wallet_handlers.emplace_back(wallet->handleShowProgress([this](const std::string& title, int nProgress) {
            ShowProgress(this, title, nProgress, /*resume_possible=*/false);
        }));
        m_connected_wallets.emplace_back(std::move(wallet));
    });
#endif
}

void SplashScreen::unsubscribeFromCoreSignals()
{
    // Disconnect signals from client
    m_handler_init_message->disconnect();
    m_handler_show_progress->disconnect();
    for (const auto& handler : m_connected_wallet_handlers) {
        handler->disconnect();
    }
    m_connected_wallet_handlers.clear();
    m_connected_wallets.clear();
}

void SplashScreen::showMessage(const QString &message, int alignment, const QColor &color)
{
    curMessage = message;
    curAlignment = alignment;
    curColor = color;
    update();
}

void SplashScreen::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.drawPixmap(0, 0, pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // Under reduced motion the timeline is pinned past its end: a
    // complete, static mesh frame with no pulse.
    const qint64 now = m_reduced_motion ? kIntroEndMs + 1 : m_clock.elapsed();
    const double pulse = (m_reduced_motion || now < kIntroEndMs)
        ? 0.0
        : kPulseAmplitude * std::sin(2.0 * M_PI * kPulseHz * (now - kIntroEndMs) / 1000.0);

    const qreal w = width();
    const qreal h = height();
    auto nodePos = [&](const MeshNode& node) {
        const qreal drift = phaseProgress(now, node.fade_start_ms, kNodeFadeMs + 500);
        const QPointF p = node.from + (node.to - node.from) * drift;
        return QPointF(p.x() * w, p.y() * h);
    };

    // Mesh edges.
    const qreal edge_alpha = phaseProgress(now, kEdgesStartMs, kEdgeFadeMs) * (0.5 + pulse);
    if (edge_alpha > 0.0) {
        QColor line = B3Theme::kAccentMuted;
        line.setAlphaF(std::clamp(edge_alpha, 0.0, 1.0));
        painter.setPen(QPen(line, 1.0));
        for (const auto& [a, b] : m_edges) {
            painter.drawLine(nodePos(m_nodes[a]), nodePos(m_nodes[b]));
        }
    }

    // Mesh nodes.
    painter.setPen(Qt::NoPen);
    for (const MeshNode& node : m_nodes) {
        const qreal alpha = phaseProgress(now, node.fade_start_ms, kNodeFadeMs) * (0.9 + pulse);
        if (alpha <= 0.0) continue;
        QColor dot = B3Theme::kAccent;
        dot.setAlphaF(std::clamp(alpha, 0.0, 1.0));
        painter.setBrush(dot);
        painter.drawEllipse(nodePos(node), 2.2, 2.2);
    }

    // The B3 mark and B3 Hive product name fade in as the mesh
    // converges.
    const qreal mark_alpha = phaseProgress(now, kMarkStartMs, kMarkFadeMs);
    if (mark_alpha > 0.0) {
        painter.setOpacity(mark_alpha);
        const QPointF center(kMeshCenter.x() * w, kMeshCenter.y() * h);
        const QSizeF markSize = QSizeF(m_mark.size()) / m_mark.devicePixelRatio();
        painter.drawPixmap(QPointF(center.x() - markSize.width() / 2.0,
                                   center.y() - markSize.height() / 2.0),
                           m_mark);
        QFont brandFont = font();
        brandFont.setPointSize(15);
        brandFont.setWeight(QFont::DemiBold);
        painter.setFont(brandFont);
        painter.setPen(B3Theme::kTextPrimary);
        painter.drawText(QRectF(0, center.y() + markSize.height() / 2.0 + 6, w, 28),
                         Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("B3 Hive"));
        painter.setOpacity(1.0);
    }

    // Real initialization messages from the node.
    QRect r = rect().adjusted(5, 5, -5, -5);
    painter.setFont(QFont(font().family(), 9));
    painter.setPen(curColor);
    painter.drawText(r, curAlignment, curMessage);
}

void SplashScreen::closeEvent(QCloseEvent *event)
{
    shutdown(); // allows an "emergency" shutdown during startup
    event->ignore();
}
