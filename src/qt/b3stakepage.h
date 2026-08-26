// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3STAKEPAGE_H
#define BITCOIN_QT_B3STAKEPAGE_H

#include <interfaces/wallet.h>

#include <QWidget>

#include <memory>

class TransactionFilterProxy;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QGridLayout;
class QResizeEvent;
class QTableView;
QT_END_NAMESPACE

/**
 * The Stake page. No staking model is exposed to Qt in this build, so
 * the page shows only what genuinely exists — the wallet's lock state,
 * its real balance, and actual reward (generated) transactions from the
 * wallet history — and states plainly that staking activation, eligible
 * balance, network weight and expectations are not available. No
 * validator or FundamentalNode UI is invented.
 */
class B3StakePage : public QWidget
{
    Q_OBJECT

public:
    explicit B3StakePage(QWidget* parent = nullptr);
    ~B3StakePage();

    //! Attach the current wallet (null for the no-wallet state).
    void setWalletModel(WalletModel* wallet_model);

public Q_SLOTS:
    void setBalance(const interfaces::WalletBalances& balances);

private Q_SLOTS:
    void updateLockState();

private:
    void resizeEvent(QResizeEvent* event) override;
    void reflowCards(int width);

    WalletModel* m_wallet_model{nullptr};
    std::unique_ptr<TransactionFilterProxy> m_rewards_filter;

    QLabel* m_backend_state{nullptr};
    QLabel* m_lock_state{nullptr};
    QLabel* m_wallet_balance{nullptr};
    QLabel* m_no_wallet{nullptr};
    QTableView* m_rewards{nullptr};
    QLabel* m_rewards_empty{nullptr};

    QGridLayout* m_card_grid{nullptr};
    QWidget* m_status_card{nullptr};
    QWidget* m_balance_card{nullptr};
    QWidget* m_rewards_card{nullptr};
    int m_layout_columns{0};
};

#endif // BITCOIN_QT_B3STAKEPAGE_H
