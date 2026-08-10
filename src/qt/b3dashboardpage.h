// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3DASHBOARDPAGE_H
#define BITCOIN_QT_B3DASHBOARDPAGE_H

#include <interfaces/wallet.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>

#include <QWidget>

#include <memory>

class PlatformStyle;
class TransactionFilterProxy;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QListView;
class QModelIndex;
class QProgressBar;
class QPushButton;
QT_END_NAMESPACE

/**
 * The B3FlowMesh wallet dashboard. A pure view over the existing models:
 * balances come from WalletModel::balanceChanged, activity from the
 * wallet's TransactionTableModel through a TransactionFilterProxy, and
 * sync/network state from ClientModel signals. Nothing is fabricated —
 * no prices, no fiat value, and the staking card states honestly that no
 * staking model is available. All amount rendering respects the display
 * unit and the existing privacy (mask values) mode.
 */
class B3DashboardPage : public QWidget
{
    Q_OBJECT

public:
    explicit B3DashboardPage(const PlatformStyle* platform_style, QWidget* parent = nullptr);
    ~B3DashboardPage();

    void setClientModel(ClientModel* client_model);
    void setWalletModel(WalletModel* wallet_model);
    void showOutOfSyncWarning(bool show);

    //! The one place dashboard money text is produced (unit-aware,
    //! privacy-aware). Exposed for tests.
    static QString formatAmount(BitcoinUnit unit, CAmount amount, bool privacy);

public Q_SLOTS:
    void setBalance(const interfaces::WalletBalances& balances);
    void setPrivacy(bool privacy);
    //! Pure view updates, driven by ClientModel signals (public so tests
    //! can exercise the rendering without a node).
    void setNumBlocks(int count, const QDateTime& block_date, double verification_progress, SyncType header, SynchronizationState sync_state);
    void setNumConnections(int count);
    void setNetworkActive(bool network_active);

Q_SIGNALS:
    //! A recent-activity row was chosen (index in the source model).
    void transactionClicked(const QModelIndex& index);
    void sendRequested();
    void receiveRequested();

private Q_SLOTS:
    void updateDisplayUnit();
    void updateEncryptionStatus();
    void updateActivityEmptyState();
    void handleActivityClicked(const QModelIndex& index);
    void setMonospacedFont(const QFont& font);

private:
    void renderBalances();
    void renderWalletState();
    QWidget* makeCard(const QString& title, QLabel** title_label_out = nullptr);

    ClientModel* m_client_model{nullptr};
    WalletModel* m_wallet_model{nullptr};
    const PlatformStyle* m_platform_style;
    std::unique_ptr<TransactionFilterProxy> m_filter;

    interfaces::WalletBalances m_balances;
    BitcoinUnit m_unit{BitcoinUnit::BTC};
    bool m_privacy{false};
    bool m_have_balances{false};

    // Balance card
    QLabel* m_available{nullptr};
    QLabel* m_pending{nullptr};
    QLabel* m_immature{nullptr};
    QLabel* m_immature_title{nullptr};
    QLabel* m_total{nullptr};

    // Wallet card
    QLabel* m_wallet_name{nullptr};
    QLabel* m_security{nullptr};
    QPushButton* m_send{nullptr};
    QPushButton* m_receive{nullptr};
    QLabel* m_no_wallet{nullptr};

    // Staking / sync / network cards
    QLabel* m_staking_note{nullptr};
    QLabel* m_sync_blocks{nullptr};
    QLabel* m_sync_time{nullptr};
    QLabel* m_sync_warning{nullptr};
    QProgressBar* m_sync_progress{nullptr};
    QLabel* m_net_peers{nullptr};
    QLabel* m_net_state{nullptr};

    // Recent activity
    QListView* m_activity{nullptr};
    QLabel* m_activity_empty{nullptr};
    QLabel* m_activity_masked{nullptr};
};

#endif // BITCOIN_QT_B3DASHBOARDPAGE_H
