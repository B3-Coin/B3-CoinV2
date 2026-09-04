// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3VALIDATORCONTROLLER_H
#define BITCOIN_QT_B3VALIDATORCONTROLLER_H

#include <consensus/amount.h>

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <cstdint>
#include <string>

class QThread;
class QTimer;
class WalletModel;

/**
 * One typed snapshot for the validator controls. Values come exclusively from
 * the wallet-scoped getstakinginfo/getfinalityinfo backends; the Qt view never
 * guesses chain state or key state.
 */
struct B3ValidatorStatus {
    bool valid{false};
    QString refresh_error;

    int tip_height{-1};
    QString next_block_phase;
    bool modern_pos_active{false};
    bool min_stake_available{false};
    CAmount min_stake_amount{0};
    int activation_depth{0};

    QString validator_key;
    QString bls_pubkey;
    bool finality_bound{false};
    bool finality_revoked{false};
    //! A binding transaction was created locally but is not yet in the chain view.
    bool binding_pending{false};
    bool current_set_member{false};
    //! Epoch-frozen eligibility weight for the next block (including bootstrap).
    uint64_t eligible_weight{0};
    uint64_t total_eligible_weight{0};
    //! Membership weight in the finality epoch set currently in force.
    uint64_t member_weight{0};
    uint64_t total_weight{0};
    uint64_t quorum_weight{0};

    CAmount active_stake{0};
    CAmount pending_stake{0};
    CAmount unconfirmed_stake{0};

    bool staking_available{false};
    bool staking_running{false};
    //! The node-global loop's actual identity (may belong to another wallet).
    QString staking_validator_key;
    bool staking_uses_this_wallet{false};
    QString staking_state;
    QString staking_last_error;
    bool finality_signing{false};
    int last_signed_height{-1};

    bool auto_corridor_mining{false};
    bool corridor_mining_attempt{false};
    QString corridor_payout_address;
    QString corridor_mining_error;
};

Q_DECLARE_METATYPE(B3ValidatorStatus)

/**
 * Qt boundary for B3 validator setup and production.
 *
 * Secret-using wallet actions execute while WalletModel::UnlockContext is in
 * scope. Read-only polling and the bounded corridor mining attempt execute on
 * one private worker thread so they cannot freeze the GUI.
 */
class B3ValidatorController : public QObject
{
    Q_OBJECT

public:
    explicit B3ValidatorController(QObject* parent = nullptr);
    ~B3ValidatorController() override;

    WalletModel* walletModel() const { return m_wallet_model; }
    const B3ValidatorStatus& currentStatus() const { return m_status; }
    bool autoCorridorMining() const { return m_auto_mining; }

public Q_SLOTS:
    void setWalletModel(WalletModel* wallet_model);
    void refresh();

    void bindFinalityKey();
    void createStake(CAmount amount);
    void startStaking();
    void stopStaking();

    //! Explicit operator opt-in. Never enabled merely by attaching a wallet.
    void startAutoCorridorMining();
    void stopAutoCorridorMining();

Q_SIGNALS:
    void statusChanged(const B3ValidatorStatus& status);
    /**
     * Structured successful result. bind_finality_key always includes txid,
     * validator_key and bls_pubkey; create_stake includes txid, amount and
     * status so the UI can present/copy them immediately.
     */
    void operationSucceeded(const QString& operation, const QVariantMap& details);
    void operationFailed(const QString& operation, const QString& error);
    void autoCorridorMiningChanged(bool active);

private:
    void executeUnlocked(const QString& operation, const std::string& method,
                         const class UniValue& params);
    void applyRefresh(uint64_t generation, B3ValidatorStatus status,
                      const QString& error);
    void queueMiningAttempt();
    void applyMiningResult(uint64_t generation, const QString& phase,
                           const QString& block_hash, const QString& error);
    std::string walletUri() const;
    void publishStatus();

    WalletModel* m_wallet_model{nullptr};
    B3ValidatorStatus m_status;
    QString m_pending_validator_key;
    QString m_pending_bls_pubkey;

    QThread* m_worker_thread{nullptr};
    QObject* m_worker{nullptr};
    QTimer* m_refresh_timer{nullptr};
    QTimer* m_mining_timer{nullptr};
    bool m_refresh_in_flight{false};
    bool m_mining_in_flight{false};
    bool m_auto_mining{false};
    QString m_mining_address;
    QString m_mining_error;
    uint64_t m_generation{0};
};

#endif // BITCOIN_QT_B3VALIDATORCONTROLLER_H
