// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3STAKEPAGE_H
#define BITCOIN_QT_B3STAKEPAGE_H

#include <qt/b3validatorcontroller.h>

#include <interfaces/wallet.h>

#include <QWidget>

#include <memory>

class BitcoinAmountField;
class TransactionFilterProxy;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QGridLayout;
class QPushButton;
class QResizeEvent;
class QTableView;
QT_END_NAMESPACE

/**
 * Operator-facing Modern-PoS setup and production page.
 *
 * All state and actions come from the same wallet RPC implementations used by
 * b3coin-cli. The page never handles private validator or BLS material: it asks
 * WalletModel for the normal unlock dialog, presents public identifiers, and
 * keeps corridor mining an explicit opt-in.
 */
class B3StakePage : public QWidget
{
    Q_OBJECT

public:
    explicit B3StakePage(QWidget* parent = nullptr);
    ~B3StakePage() override;

    //! Attach the current wallet (null for the no-wallet state).
    void setWalletModel(WalletModel* wallet_model);

public Q_SLOTS:
    void setBalance(const interfaces::WalletBalances& balances);

Q_SIGNALS:
    //! Routed to the existing current-wallet backup dialog in BitcoinGUI.
    void backupRequested();
    //! Empty hides the shell chip; otherwise this is verified controller state.
    void stakingSummaryChanged(const QString& summary);

private Q_SLOTS:
    void updateLockState();
    void setValidatorStatus(const B3ValidatorStatus& status);
    void bindFinalityKey();
    void createStake();
    void toggleStaking();
    void toggleCorridorMining();
    void operationSucceeded(const QString& operation, const QVariantMap& details);
    void operationFailed(const QString& operation, const QString& error);

private:
    void resizeEvent(QResizeEvent* event) override;
    void reflowCards(int width);
    void updateControls();
    void resetValidatorDisplay();
    void setOperationMessage(const QString& message, const QString& role = QStringLiteral("secondary"));
    void setPublicKeys(const QString& validator_key, const QString& bls_pubkey);

    WalletModel* m_wallet_model{nullptr};
    B3ValidatorController* m_controller{nullptr};
    B3ValidatorStatus m_status;
    std::unique_ptr<TransactionFilterProxy> m_rewards_filter;
    bool m_operation_busy{false};
    bool m_stake_amount_initialized{false};
    QString m_validator_key_full;
    QString m_bls_key_full;

    QLabel* m_backend_state{nullptr};
    QLabel* m_lock_state{nullptr};
    QLabel* m_chain_height{nullptr};
    QLabel* m_phase{nullptr};
    QLabel* m_validator_key{nullptr};
    QLabel* m_bls_key{nullptr};
    QLabel* m_binding_state{nullptr};
    QLabel* m_set_state{nullptr};
    QLabel* m_staking_state{nullptr};
    QLabel* m_finality_state{nullptr};

    QLabel* m_wallet_balance{nullptr};
    QLabel* m_minimum_stake{nullptr};
    QLabel* m_active_stake{nullptr};
    QLabel* m_pending_stake{nullptr};
    QLabel* m_validator_weight{nullptr};

    QPushButton* m_copy_validator{nullptr};
    QPushButton* m_copy_bls{nullptr};
    QPushButton* m_bind_finality{nullptr};
    QPushButton* m_backup{nullptr};
    BitcoinAmountField* m_stake_amount{nullptr};
    QPushButton* m_create_stake{nullptr};
    QPushButton* m_start_stop{nullptr};
    QPushButton* m_corridor_mining{nullptr};
    QLabel* m_operation_state{nullptr};

    QLabel* m_no_wallet{nullptr};
    QTableView* m_rewards{nullptr};
    QLabel* m_rewards_empty{nullptr};

    QGridLayout* m_card_grid{nullptr};
    QWidget* m_status_card{nullptr};
    QWidget* m_balance_card{nullptr};
    QWidget* m_controls_card{nullptr};
    QWidget* m_rewards_card{nullptr};
    int m_layout_columns{0};
};

#endif // BITCOIN_QT_B3STAKEPAGE_H
