// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3NAVSIDEBAR_H
#define BITCOIN_QT_B3NAVSIDEBAR_H

#include <QFrame>
#include <QMetaType>

QT_BEGIN_NAMESPACE
class QButtonGroup;
class QToolButton;
class QVBoxLayout;
QT_END_NAMESPACE

//! Canonical top-level navigation destinations of the B3FlowMesh shell.
enum class B3Page {
    Dashboard = 0,
    Trade = 1,
    Assets = 2,
    Stake = 3,
    Activity = 4,
    Settings = 5,
};
Q_DECLARE_METATYPE(B3Page)

/**
 * Left navigation sidebar. Emits navigated() when a destination is chosen;
 * it holds no application state and drives nothing directly — the shell
 * routes each destination to existing functionality.
 */
class B3NavSidebar : public QFrame
{
    Q_OBJECT

public:
    explicit B3NavSidebar(QWidget* parent = nullptr);

    //! Reflect the active page without emitting navigated().
    void setCurrentPage(B3Page page);
    B3Page currentPage() const { return m_current; }

Q_SIGNALS:
    void navigated(B3Page page);

private:
    QToolButton* addItem(B3Page page, const QString& text, const QString& objectName);

    QVBoxLayout* m_layout{nullptr};
    QButtonGroup* m_group{nullptr};
    B3Page m_current{B3Page::Dashboard};
};

#endif // BITCOIN_QT_B3NAVSIDEBAR_H
