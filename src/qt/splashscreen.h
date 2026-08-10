// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SPLASHSCREEN_H
#define BITCOIN_QT_SPLASHSCREEN_H

#include <QElapsedTimer>
#include <QPointF>
#include <QWidget>

#include <memory>
#include <utility>
#include <vector>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

class NetworkStyle;

namespace interfaces {
class Handler;
class Node;
class Wallet;
};

/** Class for the splashscreen with information of the running client.
 *
 * @note this is intentionally not a QSplashScreen. Bitcoin Core initialization
 * can take a long time, and in that case a progress window that cannot be
 * moved around and minimized has turned out to be frustrating to the user.
 */
class SplashScreen : public QWidget
{
    Q_OBJECT

public:
    explicit SplashScreen(const NetworkStyle *networkStyle);
    ~SplashScreen();
    void setNode(interfaces::Node& node);

    //! Whether the mesh animation timer is running (false under reduced
    //! motion, where a complete static frame is painted instead).
    bool animationRunning() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

public Q_SLOTS:
    /** Show message and progress */
    void showMessage(const QString &message, int alignment, const QColor &color);

    /** Handle wallet load notifications. */
    void handleLoadWallet();

protected:
    bool eventFilter(QObject * obj, QEvent * ev) override;

private:
    /** Connect core signals to splash screen */
    void subscribeToCoreSignals();
    /** Disconnect core signals to splash screen */
    void unsubscribeFromCoreSignals();
    /** Initiate shutdown */
    void shutdown();

    /** Build the static background (title, version, copyright, network). */
    void buildBackground(const NetworkStyle* networkStyle);
    /** Deterministic mesh layout (no randomness at runtime). */
    void buildMesh();
    /** Advance the animation clock; adjusts the frame rate after the
     *  intro settles into the subtle idle pulse. */
    void animationTick();
    /** 0..1 progress of one animation phase at elapsed time `now_ms`. */
    static qreal phaseProgress(qint64 now_ms, int start_ms, int duration_ms);

    // A mesh point drifting from a scattered start toward its place in
    // the converged mesh around the B3 mark.
    struct MeshNode {
        QPointF from;     // normalized [0..1] start position
        QPointF to;       // normalized [0..1] final position
        int fade_start_ms;
    };

    QPixmap pixmap;
    QPixmap m_mark;
    QString curMessage;
    QColor curColor;
    int curAlignment{0};

    std::vector<MeshNode> m_nodes;
    std::vector<std::pair<int, int>> m_edges;
    QTimer* m_anim_timer{nullptr};
    QElapsedTimer m_clock;
    bool m_reduced_motion{false};
    bool m_idle_rate{false};

    interfaces::Node* m_node = nullptr;
    bool m_shutdown = false;
    std::unique_ptr<interfaces::Handler> m_handler_init_message;
    std::unique_ptr<interfaces::Handler> m_handler_show_progress;
    std::unique_ptr<interfaces::Handler> m_handler_init_wallet;
    std::unique_ptr<interfaces::Handler> m_handler_load_wallet;
    std::list<std::unique_ptr<interfaces::Wallet>> m_connected_wallets;
    std::list<std::unique_ptr<interfaces::Handler>> m_connected_wallet_handlers;
};

#endif // BITCOIN_QT_SPLASHSCREEN_H
