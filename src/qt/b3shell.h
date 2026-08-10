// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3SHELL_H
#define BITCOIN_QT_B3SHELL_H

#include <qt/b3navsidebar.h>

#include <QWidget>

QT_BEGIN_NAMESPACE
class QStackedWidget;
QT_END_NAMESPACE

class B3NavSidebar;
class B3PlaceholderPage;
class B3TopStatus;

/**
 * The B3FlowMesh application shell: a top status area over a left
 * navigation sidebar and a stacked content area. The existing wallet UI is
 * hosted verbatim as the wallet content; Trade/Assets/Stake begin as
 * honest placeholder pages. The shell owns no application state — it
 * re-emits navigation as pageSelected() so the window routes each
 * destination to existing functionality.
 */
class B3Shell : public QWidget
{
    Q_OBJECT

public:
    explicit B3Shell(QWidget* parent = nullptr);

    //! Install the existing wallet UI as the Dashboard/Activity content.
    void setWalletWidget(QWidget* wallet_widget);

    B3TopStatus* topStatus() const { return m_topStatus; }
    B3NavSidebar* sidebar() const { return m_sidebar; }

    //! Reflect the active page and switch the content stack. Wallet-backed
    //! destinations (Dashboard/Activity) show the wallet widget; the window
    //! is responsible for the corresponding in-wallet navigation.
    void showPage(B3Page page);

    //! Replace a placeholder page's body (e.g. an honest unavailable note).
    void setTradePage(QWidget* page);
    void setAssetsPage(QWidget* page);
    void setStakePage(QWidget* page);
    //! Install an in-shell Settings page. Until one is set, selecting
    //! Settings leaves the content stack unchanged (the window opens the
    //! options dialog instead).
    void setSettingsPage(QWidget* page);
    bool hasSettingsPage() const { return m_settingsIndex >= 0; }

Q_SIGNALS:
    //! A navigation destination was chosen by the user.
    void pageSelected(B3Page page);

private:
    void replacePage(int index, QWidget* page);

    B3TopStatus* m_topStatus{nullptr};
    B3NavSidebar* m_sidebar{nullptr};
    QStackedWidget* m_content{nullptr};
    QWidget* m_walletHost{nullptr};
    int m_walletIndex{-1};
    int m_tradeIndex{-1};
    int m_assetsIndex{-1};
    int m_stakeIndex{-1};
    int m_settingsIndex{-1};
};

#endif // BITCOIN_QT_B3SHELL_H
